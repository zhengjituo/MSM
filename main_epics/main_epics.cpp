// main_epics —— IPC → EPICS CA 桥
//
// 职责边界（重要）:
//   * 本程序不启动 IOC、不生成 .db、不管 IOC 生命周期。
//   * IOC 由外部 sh 启动:  softIoc -m "P=MSM:,I=" -d msm_pvs.db
//   * 本程序是纯 CA client：轮询 IPC 取状态，按分组节拍 ca_put 到外部 IOC。
//   * PyDM 是独立进程，只连 IOC，与本程序无任何耦合。
//
// 用法:
//   msm_epics --map config/pv_map.json [--prefix-cfg config/pre.cfg]
//             [--ipc /tmp/msm_daq.sock]

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <nlohmann/json.hpp>
#include <cadef.h>

using json = nlohmann::json;

static std::atomic<bool> g_stop{false};

// ─── 前缀 / 配置 ────────────────────────────────────────────

static std::map<std::string, std::string> loadPrefixCfg(const std::string& path) {
    std::map<std::string, std::string> env;
    std::ifstream f(path);
    if (!f.is_open()) return env;
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        if (line.rfind("export ", 0) == 0) line = line.substr(7);
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(0, 1);
        if (!v.empty() && (v.front() == '"' || v.front() == '\'')) v.erase(0, 1);
        if (!v.empty() && (v.back() == '"' || v.back() == '\'')) v.pop_back();
        env[k] = v;
    }
    return env;
}

static std::string expandPrefix(std::string pv,
                                const std::map<std::string, std::string>& env) {
    auto sub = [&](const std::string& key, const std::string& def) {
        auto it = env.find(key);
        std::string val = (it != env.end()) ? it->second : def;
        std::string tok = "${" + key + "}";
        size_t pos;
        while ((pos = pv.find(tok)) != std::string::npos)
            pv.replace(pos, tok.size(), val);
    };
    sub("P", "MSM:");
    sub("I", "");
    return pv;
}

// ─── IPC client（与 main_webui 同样的协议）───────────────────

class IPCClient {
public:
    bool connect(const std::string& path) {
        if (fd_ >= 0) ::close(fd_);
        fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd_ < 0) return false;
        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
        if (::connect(fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            ::close(fd_);
            fd_ = -1;
            return false;
        }
        return true;
    }

    bool isConnected() const { return fd_ >= 0; }

    std::string send(const std::string& cmd) {
        std::lock_guard<std::mutex> lock(mux_);
        if (fd_ < 0) return R"({"status":"disconnected"})";
        std::string msg = cmd + "\n";
        if (::send(fd_, msg.data(), msg.size(), MSG_NOSIGNAL) < 0) {
            ::close(fd_);
            fd_ = -1;
            return R"({"status":"send failed"})";
        }
        char buf[4096];
        struct pollfd pfd{fd_, POLLIN | POLLERR | POLLHUP, 0};
        while (true) {
            int n = ::poll(&pfd, 1, 2000);
            if (n <= 0) break;
            if (pfd.revents & (POLLERR | POLLHUP)) {
                ::close(fd_);
                fd_ = -1;
                break;
            }
            int r = ::recv(fd_, buf, sizeof(buf) - 1, 0);
            if (r <= 0) break;
            buf[r] = 0;
            rbuf_.append(buf, r);
            if (rbuf_.find('\n') != std::string::npos) break;
        }
        std::string resp;
        size_t nl = rbuf_.find('\n');
        if (nl != std::string::npos) {
            resp = rbuf_.substr(0, nl);
            rbuf_.erase(0, nl + 1);
        }
        if (resp.empty()) resp = R"({"status":"timeout"})";
        return resp;
    }

    void close() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }

private:
    int fd_ = -1;
    std::mutex mux_;
    std::string rbuf_;
};

// ─── PV 表 ──────────────────────────────────────────────────

struct PvEntry {
    std::string from;      // "Process:x_pos"
    std::string pv;        // 展开后的 PV 名
    std::string type;      // ai / bi / bo
    std::string egu;
    int prec = 3;
    chid chan = nullptr;
    bool connected = false;
    std::string lastValue;   // 变化检测
    bool hasValue = false;
    size_t nelm = 1;      // waveform 元素数
};

struct Group {
    std::string name;
    double rateHz = 10;
    std::vector<PvEntry> vars;
    int periodMs = 1000;
    bool isWave = false;
    size_t waveNelm = 4096;
    std::vector<int> waveChannels;
};

// ─── 控制通道（PyDM → 桥 → IPC）──────────────────────────────
// 输入 PV 由桥订阅（bo / stringout），输出 PV 由桥写（bi / stringin）。
// 输入事件先进队列，主循环统一处理：一是不在 CA 回调线程里做阻塞 IPC，
// 二是保证与状态轮询共享同一个 IPCClient（该 client 不是线程安全的）。
enum CtrlKind { CK_CMD = 0, CK_SAVE = 1, CK_MSG = 2, CK_FRAME = 3 };

struct CtrlIn {
    size_t pvIdx = 0;       // 在 g_ctrlAll 里的下标
    CtrlKind kind = CK_CMD;
    std::string cmd;        // CK_CMD: 命令；CK_SAVE: 置 1 时的命令
    std::string cmdOff;     // CK_SAVE: 置 0 时的命令
    bool seenInitial = false;
    evid sub = nullptr;
};

static std::deque<PvEntry> g_ctrlAll;                      // 全部控制 PV（地址稳定）
static std::vector<CtrlIn> g_ctrlIns;                      // 输入
static std::map<std::string, size_t> g_ctrlInIdx;          // pv 名 → 下标
static std::map<std::string, PvEntry*> g_ctrlOut;          // "resp" 等 → 输出项
static std::mutex g_ctrlMux;
static std::deque<std::pair<std::string, std::string>> g_ctrlQueue;  // (pv, 值)

// ─── 值解析 ─────────────────────────────────────────────────

// 从 Status 响应里取 "Process:ch1_frames" 这类键
static std::string lookup(const json& j, const std::string& key) {
    // 直接字段
    if (j.contains(key)) {
        const auto& v = j[key];
        return v.is_string() ? v.get<std::string>() : v.dump();
    }
    // chN_feat_xxx 形式（特征表）
    return "";
}

static bool lookupVar(const json& status, const std::string& from,
                      std::string& out) {
    // from = "Process:x_pos" → 去掉模块前缀
    std::string key = from;
    auto colon = from.find(':');
    if (colon != std::string::npos) key = from.substr(colon + 1);

    // 优先带模块命名空间的键（DAQ:ch1_frames / Process:ch1_frames / Storage:...）
    if (status.contains(from)) {
        const auto& v = status[from];
        out = v.is_string() ? v.get<std::string>() : v.dump();
        return true;
    }
    if (status.contains(key)) {
        const auto& v = status[key];
        out = v.is_string() ? v.get<std::string>() : v.dump();
        return true;
    }
    // 特征表: Process:ch1_min → Status 里的 ch1_feat_min
    if (colon != std::string::npos) {
        std::string mod = from.substr(0, colon);
        if (mod == "Process") {
            // chN_min → chN_feat_min
            static const char* feats[] = {"min", "max", "sum_l", "sum_h",
                                          "vpp", "trig_id", "bunch_id",
                                          "x", "y", "i"};
            for (const char* f : feats) {
                std::string suffix = std::string("_") + f;
                if (key.size() > suffix.size() &&
                    key.compare(key.size() - suffix.size(), suffix.size(), suffix) == 0) {
                    std::string chpart = key.substr(0, key.size() - suffix.size());
                    std::string fk = chpart + "_feat_" + f;
                    // 合并表带模块命名空间：优先 Process:chN_feat_x
                    std::string nsk = mod + ":" + fk;
                    if (status.contains(nsk)) {
                        const auto& v = status[nsk];
                        out = v.is_string() ? v.get<std::string>() : v.dump();
                        return true;
                    }
                    if (status.contains(fk)) {
                        const auto& v = status[fk];
                        out = v.is_string() ? v.get<std::string>() : v.dump();
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

// ─── CA ─────────────────────────────────────────────────────

static void connectCb(struct connection_handler_args args) {
    auto* e = static_cast<PvEntry*>(ca_puser(args.chid));
    if (!e) return;
    e->connected = (args.op == CA_OP_CONN_UP);
    if (e->connected)
        std::cout << "[epics] connected: " << e->pv << "\n";
}

static bool caInit(std::vector<Group>& groups) {
    if (ca_context_create(ca_enable_preemptive_callback) != ECA_NORMAL) {
        std::cerr << "[epics] ca_context_create failed\n";
        return false;
    }
    int total = 0, ok = 0;
    for (auto& g : groups)
        for (auto& e : g.vars) {
            total++;
            int ret = ca_create_channel(e.pv.c_str(), connectCb, &e, 20, &e.chan);
            if (ret != ECA_NORMAL) {
                std::cerr << "[epics] create_channel failed: " << e.pv
                          << " (" << ca_message(ret) << ")\n";
                continue;
            }
            ok++;
        }
    // 控制通道（PyDM → 桥 / 桥 → PyDM）也走同一套连接等待
    for (auto& e : g_ctrlAll) {
        total++;
        int ret = ca_create_channel(e.pv.c_str(), connectCb, &e, 20, &e.chan);
        if (ret != ECA_NORMAL) {
            std::cerr << "[epics] create_channel failed: " << e.pv
                      << " (" << ca_message(ret) << ")\n";
            continue;
        }
        ok++;
    }
    ca_flush_io();

    // 等待连接（最长 10s）
    for (int i = 0; i < 100; i++) {
        int conn = 0;
        for (auto& g : groups)
            for (auto& e : g.vars)
                if (e.chan && ca_state(e.chan) == cs_conn) conn++;
        for (auto& e : g_ctrlAll)
            if (e.chan && ca_state(e.chan) == cs_conn) conn++;
        if (conn >= ok) break;
        ca_pend_event(0.1);
    }

    int conn = 0;
    for (auto& g : groups)
        for (auto& e : g.vars)
            if (e.chan && ca_state(e.chan) == cs_conn) {
                conn++;
                e.connected = true;
            }
    for (auto& e : g_ctrlAll)
        if (e.chan && ca_state(e.chan) == cs_conn) {
            conn++;
            e.connected = true;
        }
    std::cout << "[epics] channels connected " << conn << "/" << total << "\n";
    return conn > 0;
}

static void caShutdown(std::vector<Group>& groups) {
    for (auto& g : groups)
        for (auto& e : g.vars)
            if (e.chan) ca_clear_channel(e.chan);
    for (auto& e : g_ctrlAll)
        if (e.chan) ca_clear_channel(e.chan);
    ca_context_destroy();
}

// 常驻 channel 直接 put，不阻塞等待
static bool caPut(PvEntry& e, const std::string& value) {
    if (!e.chan || ca_state(e.chan) != cs_conn) return false;
    int ret = ECA_NORMAL;
    if (e.type == "string") {
        // stringin/stringout：CA 的 DBR_STRING 是定长字符数组（EPICS 默认 40 字节）
        std::string v = value.substr(0, 39);
        ret = ca_array_put(DBR_STRING, 1, e.chan, (void*)v.c_str());
    } else if (e.type == "bi" || e.type == "bo") {
        dbr_short_t v = (value == "1" || value == "true") ? 1 : 0;
        ret = ca_array_put(DBR_SHORT, 1, e.chan, &v);
    } else if (e.type == "longout") {
        dbr_long_t v = 0;
        try { v = (dbr_long_t)std::stol(value); } catch (...) { return false; }
        ret = ca_array_put(DBR_LONG, 1, e.chan, &v);
    } else {
        double v = 0;
        try {
            v = std::stod(value);
        } catch (...) {
            return false;
        }
        ret = ca_array_put(DBR_DOUBLE, 1, e.chan, &v);
    }
    ca_flush_io();
    return ret == ECA_NORMAL;
}

// ─── 控制通道：订阅 / 分发 ──────────────────────────────────

static void ctrlEventCb(struct event_handler_args args) {
    auto* e = static_cast<PvEntry*>(ca_puser(args.chid));
    if (!e) return;
    std::string val;
    if (args.type == DBR_STRING) {
        val = (const char*)args.dbr;
    } else if (args.type == DBR_SHORT) {
        val = std::to_string(*(const dbr_short_t*)args.dbr);
    } else if (args.type == DBR_LONG) {
        val = std::to_string(*(const dbr_long_t*)args.dbr);
    } else if (args.type == DBR_DOUBLE) {
        char b[32];
        snprintf(b, sizeof b, "%g", *(const double*)args.dbr);
        val = b;
    }
    std::lock_guard<std::mutex> lk(g_ctrlMux);
    g_ctrlQueue.emplace_back(e->pv, val);
}

static void ctrlSubscribe() {
    for (size_t i = 0; i < g_ctrlIns.size(); i++) {
        CtrlIn& in = g_ctrlIns[i];
        PvEntry& e = g_ctrlAll[in.pvIdx];
        if (!e.chan || ca_state(e.chan) != cs_conn) {
            std::cerr << "[epics] control pv not connected: " << e.pv << "\n";
            continue;
        }
        long reqType = (in.kind == CK_MSG)   ? DBR_STRING
                     : (in.kind == CK_FRAME) ? DBR_LONG
                                             : DBR_SHORT;
        int ret = ca_create_subscription(reqType, 1, e.chan, DBE_VALUE,
                                         ctrlEventCb, nullptr, &in.sub);
        if (ret != ECA_NORMAL)
            std::cerr << "[epics] subscribe failed: " << e.pv
                      << " (" << ca_message(ret) << ")\n";
    }
    ca_flush_io();
}

// 处理队列里的控制事件。不能在 CA 回调线程里发 IPC（会阻塞网络回调），
// 所以统一在主循环做。
static int ctrlDrain(IPCClient& ipc) {
    std::deque<std::pair<std::string, std::string>> batch;
    {
        std::lock_guard<std::mutex> lk(g_ctrlMux);
        batch.swap(g_ctrlQueue);
    }
    int n = 0;
    for (auto& ev : batch) {
        const std::string& pv = ev.first;
        const std::string& val = ev.second;
        auto it = g_ctrlInIdx.find(pv);
        if (it == g_ctrlInIdx.end()) continue;
        CtrlIn& in = g_ctrlIns[it->second];
        PvEntry& e = g_ctrlAll[in.pvIdx];

        // 订阅会立刻投递一次初值：初值不是用户操作，全部跳过
        if (!in.seenInitial) {
            in.seenInitial = true;
            continue;
        }

        std::string cmd;
        if (in.kind == CK_CMD) {
            if (val != "1") continue;
            cmd = in.cmd;
        } else if (in.kind == CK_SAVE) {
            cmd = (val == "1") ? in.cmd : in.cmdOff;
        } else if (in.kind == CK_FRAME) {
            long idx = 0;
            try { idx = std::stol(val); } catch (...) { continue; }
            // -1 = 自动对齐 100ms 边界（webui 的 Index 输入框 min 同为 -1），
            // 必须放行；其余负数视为输入错误丢弃
            if (idx < -1) continue;
            // 走 Module 内建的 SetVar，直接写 VarTable 里的 frame_index
            // （atomic），一次写入即生效，不依赖额外消息命令。
            cmd = "MSG SetVar >> var=frame_index,value=" + std::to_string(idx) + " -@ Process";
        } else {  // CK_MSG
            if (val.empty()) continue;
            cmd = val;
        }

        std::string resp = ipc.send(cmd);
        std::cout << "[epics] ctrl " << pv << " -> " << cmd
                  << " : " << resp << std::endl;

        // 触发型 bo 复位；msg 清空。frame_index 不复位：它表达的是"当前请求
        // 的帧号状态"而非触发（相同值重发 = 无操作，语义正确）。若复位到 -1，
        // 桥自己的复位事件会再次分发 SetVar -1，把刚设好的帧号打回自动模式
        // （实测 500 只活一个 batch）。
        if (in.kind == CK_CMD) caPut(e, "0");
        else if (in.kind == CK_MSG) caPut(e, "");

        auto rit = g_ctrlOut.find("resp");
        if (rit != g_ctrlOut.end() && rit->second)
            caPut(*rit->second, resp.substr(0, 39));
        n++;
    }
    return n;
}

static void ctrlPublishStatus(const std::string& key, const std::string& value) {
    auto it = g_ctrlOut.find(key);
    if (it == g_ctrlOut.end() || !it->second) return;
    PvEntry& e = *it->second;
    if (e.hasValue && e.lastValue == value) return;
    if (caPut(e, value)) {
        e.lastValue = value;
        e.hasValue = true;
    }
}

// ─── Base64 / waveform ─────────────────────────────────────────

static int b64v(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static std::vector<unsigned char> b64decode(const std::string& in) {
    // 标准实现：valb 是当前累积的“多余位数”，取值 0..5 或初始 -8。
    // 注意必须用 (val >> bits)，写成 (bits - 8) 在 bits=4 时是移位 -4（UB），
    // 会把解出来的字节序列整体错位。
    std::vector<unsigned char> out;
    int val = 0, bits = -8;
    for (unsigned char c : in) {
        int d = b64v(c);
        if (d < 0) continue;
        val = (val << 6) + d;
        bits += 6;
        if (bits >= 0) {
            out.push_back((unsigned char)((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

// 推送一条波形到 waveform record（ULONG 元素）。
// data 是 little-endian uint16 序列。
//
// 只写实际长度 n（不是写满 nelm）：CA 写数组时 element count 就是 NORD，
// 若写满 nelm，NORD 恒为 4096，PyDM 横坐标就被拉到 0..4095，而真实采样
// 只有前 TrigLength 点，曲线会挤在最左边一小条。NELM（记录容量）仍由 .db
// 固定，不受影响。
static bool caPutWave(PvEntry& e, const std::vector<unsigned char>& raw) {
    if (!e.chan || ca_state(e.chan) != cs_conn) return false;
    size_t n = raw.size() / sizeof(uint16_t);
    if (n > e.nelm) n = e.nelm;
    if (n == 0) return true;             // 没有数据就别把 NORD 打成 0

    static std::vector<uint32_t> buf;
    buf.assign(n, 0);
    for (size_t i = 0; i < n; i++) {
        const unsigned char* p = &raw[i * 2];
        buf[i] = (uint32_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
    }
    int ret = ca_array_put(DBR_LONG, n, e.chan, buf.data());
    ca_flush_io();
    if (std::getenv("MSM_EPICS_DEBUG"))
        std::cerr << "[epics] put-wave " << e.pv << " n=" << n << "\n";
    return ret == ECA_NORMAL;
}

// ─── main ───────────────────────────────────────────────────

int main(int argc, char** argv) {
    std::string mapPath = "config/pv_map.json";
    std::string prefixCfg = "config/pre.cfg";
    std::string ipcPath;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--map" && i + 1 < argc) mapPath = argv[++i];
        else if (a == "--prefix-cfg" && i + 1 < argc) prefixCfg = argv[++i];
        else if (a == "--ipc" && i + 1 < argc) ipcPath = argv[++i];
        else if (a == "-h" || a == "--help") {
            std::cout << "usage: msm_epics [--map FILE] [--prefix-cfg FILE] [--ipc PATH]\n";
            return 0;
        }
    }

    signal(SIGINT, [](int) { g_stop = true; });
    signal(SIGTERM, [](int) { g_stop = true; });
    signal(SIGPIPE, SIG_IGN);

    // 前缀优先级：环境变量 > pre.cfg
    auto env = loadPrefixCfg(prefixCfg);
    if (const char* p = getenv("MSM_P")) env["P"] = p;
    if (const char* i = getenv("MSM_I")) env["I"] = i;

    json cfg;
    try {
        std::ifstream f(mapPath);
        if (!f.is_open()) {
            std::cerr << "[epics] cannot open " << mapPath << "\n";
            return 1;
        }
        f >> cfg;
    } catch (const std::exception& e) {
        std::cerr << "[epics] pv_map parse error: " << e.what() << "\n";
        return 1;
    }

    if (ipcPath.empty()) ipcPath = cfg.value("ipc", "/tmp/msm_daq.sock");
    std::string addrList = cfg.value("ioc", json::object()).value("addr_list", "127.0.0.1:5064");
    setenv("EPICS_CA_ADDR_LIST", addrList.c_str(), 0);
    setenv("EPICS_CA_AUTO_ADDR_LIST", "NO", 0);

    std::vector<Group> groups;
    for (auto& [gname, gj] : cfg["groups"].items()) {
        if (gname.size() && gname[0] == '_') continue;
        Group g;
        g.name = gname;
        g.rateHz = gj.value("rate_hz", 10.0);
        if (g.rateHz < 2.0) g.rateHz = 2.0;
        if (g.rateHz > 10.0) g.rateHz = 10.0;
        g.periodMs = (int)(1000.0 / g.rateHz);
        if (g.periodMs < 1) g.periodMs = 1;

        for (auto& item : gj["vars"]) {
            PvEntry e;
            e.from = item["from"];
            e.pv = expandPrefix(item["pv"], env);
            e.type = item.value("kind", "ai");
            e.egu = item.value("egu", "");
            e.prec = item.value("prec", 0);
            g.vars.push_back(e);
        }

        // 波形分组：为每个通道建一条 waveform PV
        g.waveNelm = gj.value("wave_nelm", 4096);
        if (gj.contains("channels")) {
            std::string pvPat = gj.value("pv_pattern",
                                         std::string("${P}${I}Process:wave_ch{ch}"));
            for (auto& ch : gj["channels"]) {
                int c = ch.get<int>();
                std::string pvName = pvPat;
                size_t pos = pvName.find("{ch}");
                if (pos != std::string::npos)
                    pvName.replace(pos, 4, std::to_string(c));
                PvEntry e;
                e.from = "Process:wave_ch" + std::to_string(c);
                e.pv = expandPrefix(pvName, env);
                e.type = "wave";
                e.nelm = g.waveNelm;
                g.vars.push_back(e);
                g.waveChannels.push_back(c);
            }
            g.isWave = true;
        }

        if (!g.vars.empty()) {
            std::cout << "[epics] group " << g.name << " @ " << g.rateHz
                      << "Hz (" << g.vars.size() << " vars)\n";
            groups.push_back(std::move(g));
        }
    }

    // ─── 控制通道（PyDM → 桥 → IPC；桥 → PyDM 状态）──────────
    {
        auto cj = cfg.value("control", json::object());

        auto addOut = [&](const std::string& key, const std::string& pvName,
                          const std::string& type) {
            PvEntry e;
            e.from = key;
            e.pv = expandPrefix(pvName, env);
            e.type = type;
            g_ctrlAll.push_back(e);
            g_ctrlOut[key] = &g_ctrlAll.back();   // deque: push_back 不会失效
        };

        for (auto& item : cj.value("commands", json::array())) {
            PvEntry e;
            e.from = "CTRL:cmd";
            e.pv = expandPrefix(item.value("pv", ""), env);
            e.type = "bo";
            g_ctrlAll.push_back(e);
            CtrlIn in;
            in.pvIdx = g_ctrlAll.size() - 1;
            in.kind = CK_CMD;
            in.cmd = item.value("cmd", "");
            g_ctrlInIdx[e.pv] = g_ctrlIns.size();
            g_ctrlIns.push_back(in);
        }

        if (cj.contains("save_pv")) {
            auto& sp = cj["save_pv"];
            PvEntry e;
            e.from = "CTRL:save_enable";
            e.pv = expandPrefix(sp.value("pv", ""), env);
            e.type = "bo";
            g_ctrlAll.push_back(e);
            CtrlIn in;
            in.pvIdx = g_ctrlAll.size() - 1;
            in.kind = CK_SAVE;
            in.cmd = sp.value("on", "ENABLESAVE");
            in.cmdOff = sp.value("off", "DISABLESAVE");
            g_ctrlInIdx[e.pv] = g_ctrlIns.size();
            g_ctrlIns.push_back(in);
        }

        if (cj.contains("msg_pv")) {
            auto& mp = cj["msg_pv"];
            PvEntry e;
            e.from = "CTRL:msg";
            e.pv = expandPrefix(mp.value("pv", ""), env);
            e.type = "string";
            g_ctrlAll.push_back(e);
            CtrlIn in;
            in.pvIdx = g_ctrlAll.size() - 1;
            in.kind = CK_MSG;
            g_ctrlInIdx[e.pv] = g_ctrlIns.size();
            g_ctrlIns.push_back(in);
        }

        if (cj.contains("frame_index_pv")) {
            auto& fp = cj["frame_index_pv"];
            PvEntry e;
            e.from = "CTRL:set_frame_index";
            e.pv = expandPrefix(fp.value("pv", ""), env);
            e.type = "longout";
            g_ctrlAll.push_back(e);
            CtrlIn in;
            in.pvIdx = g_ctrlAll.size() - 1;
            in.kind = CK_FRAME;
            g_ctrlInIdx[e.pv] = g_ctrlIns.size();
            g_ctrlIns.push_back(in);
        }

        if (cj.contains("status")) {
            auto& sj = cj["status"];
            if (sj.contains("backend_alive") && sj["backend_alive"].is_string())
                addOut("backend_alive", sj["backend_alive"].get<std::string>(), "bi");
            if (sj.contains("daq_running") && sj["daq_running"].is_string())
                addOut("daq_running", sj["daq_running"].get<std::string>(), "bi");
            if (sj.contains("resp") && sj["resp"].is_string())
                addOut("resp", sj["resp"].get<std::string>(), "string");
        }

        if (!g_ctrlIns.empty() || !g_ctrlOut.empty())
            std::cout << "[epics] control: " << g_ctrlIns.size()
                      << " input(s), " << g_ctrlOut.size() << " output(s)\n";
    }

    if (groups.empty()) {
        std::cerr << "[epics] no PVs configured\n";
        return 1;
    }

    if (!caInit(groups)) {
        std::cerr << "[epics] no channel connected — is the IOC running?\n";
        caShutdown(groups);
        return 1;
    }

    ctrlSubscribe();

    IPCClient ipc;
    bool ipcOk = ipc.connect(ipcPath);
    if (!ipcOk)
        std::cerr << "[epics] IPC not available yet: " << ipcPath
                  << " (will retry)\n";

    std::cout << "[epics] running; ipc=" << ipcPath << std::endl;

    auto nextDue = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point lastIpcRetry = std::chrono::steady_clock::now();

    // 每个分组独立计时
    std::vector<std::chrono::steady_clock::time_point> due(groups.size(),
                                                           std::chrono::steady_clock::now());

    while (!g_stop.load()) {
        auto now = std::chrono::steady_clock::now();

        // 先处理 PyDM 下发的控制命令（START/STOP/QUIT/SetFrameIndex...）
        ctrlDrain(ipc);

        // IPC 重连
        if (!ipc.isConnected()) {
            if (now - lastIpcRetry > std::chrono::seconds(2)) {
                lastIpcRetry = now;
                if (ipc.connect(ipcPath))
                    std::cout << "[epics] IPC reconnected\n";
            }
        }

        // 取一次状态（1s 轮询，足够 — 推送节拍独立）
        // 轮询三个 backend 模块并合并到一张扁平表。
        // 键冲突说明同名变量被多个模块暴露，此时以第一个为准。
        json status = json::object();
        if (ipc.isConnected()) {
            static const char* mods[] = {"DAQModule", "Process", "Storage"};
            for (const char* m : mods) {
                std::string r = ipc.send(std::string("MSG Status >> -@ ") + m);
                json j;
                try {
                    j = json::parse(r);
                } catch (...) {
                    continue;
                }
                if (!j.is_object()) continue;
                std::string ns = (std::string(m) == "DAQModule") ? "DAQ" : m;
                for (auto& [k, v] : j.items()) {
                    if (v.is_structured()) continue;   // 跳过嵌套对象
                    std::string nk = ns + ":" + k;
                    if (!status.contains(nk)) status[nk] = v;
                }
            }
        }

        // 控制状态（状态灯）：每轮都算，变化才写
        {
            ctrlPublishStatus("backend_alive", ipc.isConnected() ? "1" : "0");
            bool running = false;
            auto rit = status.find("DAQ:running");
            if (rit != status.end()) {
                std::string s = rit->is_string() ? rit->get<std::string>()
                                                 : rit->dump();
                running = (s == "true" || s == "1");
            }
            ctrlPublishStatus("daq_running", running ? "1" : "0");
        }

        // 各分组按自己的 deadline 推送
        for (size_t gi = 0; gi < groups.size(); gi++) {
            if (now < due[gi]) continue;
            due[gi] = now + std::chrono::milliseconds(groups[gi].periodMs);
            for (auto& e : groups[gi].vars) {
                if (e.type == "wave") {
                    // from = "Process:wave_chN" → Status 里 chN_feat_wave_raw (base64)
                    std::string chPart = e.from.substr(e.from.rfind("ch") + 2);
                    std::string key = "ch" + chPart + "_feat_wave_raw";
                    // 合并表带命名空间，优先 Process:chN_feat_wave_raw
                    std::string nsKey = "Process:" + key;
                    auto it = status.find(nsKey);
                    if (it == status.end()) it = status.find(key);
                    if (it == status.end() || !it->is_string()) continue;
                    const std::string& b64 = it->get_ref<const std::string&>();
                    if (b64.empty() || (e.hasValue && b64 == e.lastValue)) continue;
                    auto raw = b64decode(b64);
                    if (caPutWave(e, raw)) {
                        e.lastValue = b64;
                        e.hasValue = true;
                    }
                    continue;
                }
                std::string val;
                if (!lookupVar(status, e.from, val)) continue;
                if (e.hasValue && val == e.lastValue) continue;
                if (caPut(e, val)) {
                    e.lastValue = val;
                    e.hasValue = true;
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "[epics] stopping\n";
    ipc.close();
    caShutdown(groups);
    return 0;
}
