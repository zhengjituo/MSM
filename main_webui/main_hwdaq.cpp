#include "httplib.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/poll.h>
#include <sys/wait.h>
#include <signal.h>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <sstream>
#include <fstream>
#include <cstring>
#include <unistd.h>
#include <vector>

#ifndef _WIN32
  #define JSON_USE_IMPLICIT_CONVERSIONS 0
  #define NLOHMANN_JSON_ABI_VERSION 3
#endif
#include "nlohmann/json.hpp"

using nlohmann::json;

static std::atomic<bool> g_stop{false};

static const char* IPC_PATH = "/tmp/msm_daq.sock";

// ── Backend process management ──────────────────────────────────

static pid_t g_childPid = -1;
static std::string g_backendMode;
enum BackendStatus { BS_IDLE, BS_LAUNCHING, BS_RUNNING, BS_CRASHED };
static BackendStatus g_backendStatus = BS_IDLE;
static std::mutex g_backendMtx;

static std::string findCliPath(const std::string& binary) {
    const char* envDir = ::getenv("MSM_CLI_PATH");
    if (envDir && envDir[0]) {
        std::string p = std::string(envDir) + "/" + binary;
        if (::access(p.c_str(), X_OK) == 0) return p;
    }
    char exe[1024];
    ssize_t len = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (len > 0) {
        exe[len] = 0;
        std::string ep(exe);
        size_t s = ep.rfind('/');
        if (s != std::string::npos) {
            // If webui is at .../main_webui/build/msm_webui, look for
            // .../main_cli/build/msm_cli_*
            std::string base = ep.substr(0, s);
            // Try: ../../main_cli/build/binary
            std::string candidate = base + "/../../main_cli/build/" + binary;
            std::string norm;
            // Resolve ".." simply
            size_t pos;
            while ((pos = candidate.find("/../")) != std::string::npos) {
                if (pos == 0) { candidate.erase(0, 3); continue; }
                size_t prev = candidate.rfind('/', pos - 1);
                if (prev == std::string::npos) prev = 0;
                candidate.erase(prev, pos + 3 - prev);
            }
            if (::access(candidate.c_str(), X_OK) == 0) return candidate;
        }
    }
    return binary;
}

static bool launchBackend(const std::string& mode) {
    std::lock_guard<std::mutex> lock(g_backendMtx);
    if (g_childPid > 0) {
        int st = 0;
        if (::waitpid(g_childPid, &st, WNOHANG) == 0)
            return false; // already running
    }
    std::string binary = (mode == "sim") ? "msm_cli_simsrc" : "msm_cli_hwdaq";
    std::string path = findCliPath(binary);

    pid_t pid = ::fork();
    if (pid < 0) return false;
    if (pid == 0) {
        // Child
        ::setsid();
        ::execlp(path.c_str(), path.c_str(), nullptr);
        ::_exit(127);
    }
    g_childPid = pid;
    g_backendMode = mode;
    g_backendStatus = BS_LAUNCHING;
    return true;
}

static void stopBackend() {
    pid_t pid;
    {
        std::lock_guard<std::mutex> lock(g_backendMtx);
        pid = g_childPid;
        if (pid <= 0) return;
        g_childPid = -1;
        g_backendStatus = BS_IDLE;
        g_backendMode.clear();
    }
    ::kill(pid, SIGTERM);
    // Give it a moment, then SIGKILL
    for (int i = 0; i < 10; i++) {
        int st = 0;
        if (::waitpid(pid, &st, WNOHANG) == pid) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);
}

static void checkChild() {
    std::lock_guard<std::mutex> lock(g_backendMtx);
    if (g_childPid <= 0) return;
    int st = 0;
    pid_t r = ::waitpid(g_childPid, &st, WNOHANG);
    if (r == g_childPid) {
        g_childPid = -1;
        g_backendStatus = (g_backendStatus == BS_LAUNCHING) ? BS_IDLE : BS_CRASHED;
    }
}

// ── IPC 客户端 ──────────────────────────────────────────────────

class IPCClient {
    int fd_ = -1;
    std::mutex mux_;
    std::string rbuf_;
    bool connected() const { return fd_ >= 0; }
public:
    bool connect(const char* path = IPC_PATH) {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
        fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd_ < 0) return false;
        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
        if (::connect(fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            ::close(fd_); fd_ = -1; return false;
        }
        return true;
    }
    bool isConnected() { return fd_ >= 0; }
    std::string send(const std::string& cmd) {
        std::lock_guard<std::mutex> lock(mux_);
        if (!connected()) return R"({"status":"disconnected"})";
        std::string msg = cmd + "\n";
        ssize_t sent = ::send(fd_, msg.data(), msg.size(), MSG_NOSIGNAL);
        if (sent < 0) { ::close(fd_); fd_ = -1; return R"({"status":"send failed"})"; }
        char buf[4096];
        struct pollfd pfds[1] = {{fd_, POLLIN | POLLERR | POLLHUP, 0}};
        while (true) {
            int n = ::poll(pfds, 1, 2000);
            if (n <= 0) break;
            if (pfds[0].revents & (POLLERR | POLLHUP)) { ::close(fd_); fd_ = -1; break; }
            int r = ::recv(fd_, buf, sizeof(buf) - 1, 0);
            if (r <= 0) break;
            buf[r] = 0;
            rbuf_.append(buf, r);
            if (rbuf_.find('\n') != std::string::npos) break;
        }
        std::string response;
        size_t nl = rbuf_.find('\n');
        if (nl != std::string::npos) {
            response = rbuf_.substr(0, nl);
            rbuf_.erase(0, nl + 1);
        }
        if (response.empty())
            response = R"({"status":"timeout"})";
        return response;
    }
    void close() { if (connected()) { ::close(fd_); fd_ = -1; } }
};

// ── 状态缓存 ─────────────────────────────────────────────────────

struct FeatureRow {
    int64_t trig_id = 0;
    uint64_t bunch_id = 0;
    double min_val = 0, max_val = 0, sum_l = 0, sum_h = 0, vpp = 0;
    double x = 0, y = 0, i_val = 0;
};

struct ChannelStatus {
    int64_t daq_frames = 0;
    int64_t proc_frames = 0;
    int64_t proc_batches = 0;
    int64_t saved_frames = 0;
};

struct AppSnapshot {
    bool running = false;
    bool saveEnabled = false;
    int channels = 0;
    ChannelStatus ch[16];
    int64_t batchNo = 0;
    int64_t frameIndex = -1;
    FeatureRow feat[16];
    bool hasFeature = false;
    // Backend fields
    bool backendAlive = false;
    std::string backendMode;
    int backendPid = 0;
    std::string backendStatusStr;
};
static AppSnapshot g_snap;
static std::mutex g_snapMtx;
static std::condition_variable g_snapCv;
static bool g_snapUpdated = false;

static double parseDouble(const json& j, const std::string& key) {
    try { return std::stod(j.value(key, "0")); } catch (...) { return 0.0; }
}

static int64_t parseInt64(const json& j, const std::string& key) {
    try { return std::stoll(j.value(key, "0")); } catch (...) { return 0; }
}

static AppSnapshot parseSnapshot(json& daqJ, json& procJ, json& storJ, bool saveEnabled) {
    AppSnapshot snap;
    snap.saveEnabled = saveEnabled;
    snap.running = daqJ.value("running", "false") == "true";

    int nCh = 0;
    for (int i = 0; i < 16; i++) {
        std::string idx = std::to_string(i + 1);
        std::string dk = "ch" + idx + "_frames";
        std::string pk = "ch" + idx + "_frames";
        std::string pb = "ch" + idx + "_batches";
        std::string sk = "ch" + idx + "_saved_frames";

        if (!daqJ.contains(dk)) break;
        nCh = i + 1;
        auto& c = snap.ch[i];
        c.daq_frames = std::stoll(daqJ.value(dk, "0"));
        c.proc_frames = std::stoll(procJ.value(pk, "0"));
        c.proc_batches = std::stoll(procJ.value(pb, "0"));
        c.saved_frames = std::stoll(storJ.value(sk, "0"));
    }
    snap.channels = nCh;

    // Feature data
    snap.batchNo = parseInt64(procJ, "batch_no");
    snap.frameIndex = parseInt64(procJ, "frame_index");
    snap.hasFeature = procJ.contains("batch_no") && snap.batchNo > 0;
    for (int i = 0; i < nCh; i++) {
        std::string p = "ch" + std::to_string(i + 1) + "_feat_";
        if (!procJ.contains(p + "trig_id")) continue;
        auto& f = snap.feat[i];
        f.trig_id = parseInt64(procJ, p + "trig_id");
        f.bunch_id = (uint64_t)parseInt64(procJ, p + "bunch_id");
        f.min_val = parseDouble(procJ, p + "min");
        f.max_val = parseDouble(procJ, p + "max");
        f.sum_l = parseDouble(procJ, p + "sum_l");
        f.sum_h = parseDouble(procJ, p + "sum_h");
        f.vpp = parseDouble(procJ, p + "vpp");
        f.x = parseDouble(procJ, p + "x");
        f.y = parseDouble(procJ, p + "y");
        f.i_val = parseDouble(procJ, p + "i");
    }
    // Backend status
    {
        std::lock_guard<std::mutex> lock(g_backendMtx);
        snap.backendAlive = g_childPid > 0;
        snap.backendPid = g_childPid > 0 ? g_childPid : 0;
        snap.backendMode = g_backendMode;
        switch (g_backendStatus) {
            case BS_IDLE:     snap.backendStatusStr = "stopped"; break;
            case BS_LAUNCHING: snap.backendStatusStr = "launching"; break;
            case BS_RUNNING:  snap.backendStatusStr = "running"; break;
            case BS_CRASHED:  snap.backendStatusStr = "crashed"; break;
        }
    }
    return snap;
}

static std::string buildSseData(const AppSnapshot& snap) {
    std::ostringstream o;
    o << "data: {";
    o << "\"running\":" << (snap.running ? "true" : "false") << ",";
    o << "\"save_enabled\":" << (snap.saveEnabled ? "true" : "false");
    o << ",\"batch_no\":" << snap.batchNo;
    o << ",\"frame_index\":" << snap.frameIndex;
    o << ",\"has_feature\":" << (snap.hasFeature ? "true" : "false");
    o << ",\"backend_alive\":" << (snap.backendAlive ? "true" : "false");
    o << ",\"backend_pid\":" << snap.backendPid;
    o << ",\"backend_mode\":\"" << snap.backendMode << "\"";
    o << ",\"backend_status\":\"" << snap.backendStatusStr << "\"";
    o << ",\"channels\":[";
    for (int i = 0; i < snap.channels; i++) {
        if (i > 0) o << ",";
        auto& c = snap.ch[i];
        o << "{";
        o << "\"ch\":" << (i + 1);
        o << ",\"daq_frames\":" << c.daq_frames;
        o << ",\"proc_frames\":" << c.proc_frames;
        o << ",\"proc_batches\":" << c.proc_batches;
        o << ",\"saved_frames\":" << c.saved_frames;
        o << "}";
    }
    o << "],\"features\":[";
    for (int i = 0; i < snap.channels; i++) {
        if (i > 0) o << ",";
        auto& f = snap.feat[i];
        o << "{";
        o << "\"ch\":" << (i + 1);
        o << ",\"trig_id\":" << f.trig_id;
        o << ",\"bunch_id\":" << f.bunch_id;
        o << ",\"min\":" << f.min_val;
        o << ",\"max\":" << f.max_val;
        o << ",\"sum_l\":" << f.sum_l;
        o << ",\"sum_h\":" << f.sum_h;
        o << ",\"vpp\":" << f.vpp;
        o << ",\"x\":" << f.x;
        o << ",\"y\":" << f.y;
        o << ",\"i\":" << f.i_val;
        o << "}";
    }
    o << "]}\n\n";
    return o.str();
}

// ── 主程序 ─────────────────────────────────────────────────────

int main() {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, [](int) { g_stop.store(true); });
    signal(SIGTERM, [](int) { g_stop.store(true); });

    IPCClient ipc;
    // Try to connect in case backend was started externally
    ipc.connect(IPC_PATH);

    bool saveEnabled = false;
    bool ipcWasConnected = ipc.isConnected();

    httplib::Server svr;

    svr.Post("/api/msg", [&](const httplib::Request& req, httplib::Response& res) {
        std::string cmd = req.body;
        if (cmd.empty()) {
            res.set_content(R"({"status":"empty"})", "application/json");
            return;
        }
        std::string response;
        if (cmd == "start") {
            response = ipc.send("START");
        } else if (cmd == "stop") {
            response = ipc.send("STOP");
        } else if (cmd == "enablesave") {
            ipc.send("ENABLESAVE");
            saveEnabled = true;
            response = R"({"status":"save enabled"})";
        } else if (cmd == "disablesave") {
            ipc.send("DISABLESAVE");
            saveEnabled = false;
            response = R"({"status":"save disabled"})";
        } else if (cmd == "reload") {
            response = ipc.send("RELOAD");
        } else if (cmd == "quit") {
            ipc.send("QUIT");
            response = R"({"status":"quitting"})";
        } else {
            response = ipc.send("MSG " + cmd);
        }
        res.set_content(response, "application/json");
    });

    svr.Post("/api/run", [&](const httplib::Request& req, httplib::Response& res) {
        std::string mode = req.body;
        if (mode != "hw" && mode != "sim") {
            res.set_content(R"json({"status":"invalid mode (hw or sim)"})json", "application/json");
            return;
        }
        {
            std::lock_guard<std::mutex> lock(g_backendMtx);
            if (g_childPid > 0) {
                int st = 0;
                if (::waitpid(g_childPid, &st, WNOHANG) == 0) {
                    res.set_content(R"({"status":"already running"})", "application/json");
                    return;
                }
            }
        }
        if (!launchBackend(mode)) {
            res.set_content(R"({"status":"launch failed"})", "application/json");
            return;
        }
        // Retry IPC connection until timeout
        bool ok = false;
        for (int i = 0; i < 30; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (ipc.connect(IPC_PATH)) { ok = true; break; }
            checkChild(); // detect early crash
            std::lock_guard<std::mutex> lock(g_backendMtx);
            if (g_childPid <= 0) break;
        }
        if (ok) {
            // Wait for DAQ module to respond with running field (initialized)
            bool daqReady = false;
            for (int i = 0; i < 30; i++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                std::string r = ipc.send("MSG Status >> -@ DAQModule");
                checkChild();
                std::lock_guard<std::mutex> lock(g_backendMtx);
                if (g_childPid <= 0) break;
                try {
                    json j = json::parse(r);
                    if (j.contains("running")) { daqReady = true; break; }
                } catch (...) {}
            }
            {
                std::lock_guard<std::mutex> lock(g_backendMtx);
                g_backendStatus = daqReady ? BS_RUNNING : BS_CRASHED;
            }
            res.set_content(daqReady
                ? R"({"status":"ready"})"
                : R"({"status":"DAQ not ready"})", "application/json");
        } else {
            res.set_content(R"({"status":"IPC timeout"})", "application/json");
        }
    });

    svr.Post("/api/stop", [&](const httplib::Request&, httplib::Response& res) {
        ipc.send("QUIT");
        stopBackend();
        ipc.close();
        res.set_content(R"({"status":"stopped"})", "application/json");
    });

    svr.Get("/api/events", [&](const httplib::Request&, httplib::Response& res) {
        res.set_chunked_content_provider("text/event-stream",
            [&](size_t, httplib::DataSink& sink) {
                std::unique_lock<std::mutex> lock(g_snapMtx);
                if (!g_snapUpdated) {
                    g_snapCv.wait_for(lock, std::chrono::milliseconds(500));
                    if (!g_snapUpdated) return !g_stop.load();
                }
                std::string sse = buildSseData(g_snap);
                sink.os << sse;
                sink.os.flush();
                g_snapUpdated = false;
                return !g_stop.load();
            });
    });

    svr.Get("/api/status", [](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(g_snapMtx);
        res.set_content(buildSseData(g_snap), "application/json");
    });

    svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
        std::string htmlPath;
        char exe[1024];
        ssize_t len = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (len > 0) {
            exe[len] = 0;
            std::string ep(exe);
            size_t s = ep.rfind('/');
            htmlPath = (s != std::string::npos ? ep.substr(0, s) : ".") + "/../web/msm_daq_hw.html";
        } else {
            htmlPath = "web/msm_daq_hw.html";
        }
        std::ifstream f(htmlPath);
        if (!f.is_open()) { res.status = 404; res.set_content("not found", "text/plain"); return; }
        res.set_content(std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()), "text/html");
    });

    std::thread httpThread([&] {
        std::cout << "[webui] http://localhost:8080" << std::endl;
        svr.listen("0.0.0.0", 8080);
    });

    // Poll loop
    while (!g_stop.load()) {
        checkChild();
        json daqJ, procJ, storJ;

        std::string dr = ipc.send("MSG Status >> -@ DAQModule");
        std::string pr = ipc.send("MSG Status >> -@ Process");
        std::string sr = ipc.send("MSG Status >> -@ Storage");
        try { daqJ = json::parse(dr); } catch (...) {}
        try { procJ = json::parse(pr); } catch (...) {}
        try { storJ = json::parse(sr); } catch (...) {}

        json statusOnly; statusOnly["status"] = "timeout";
        if (daqJ.empty()) daqJ = statusOnly;
        if (procJ.empty()) procJ = statusOnly;
        if (storJ.empty()) storJ = statusOnly;

        AppSnapshot snap = parseSnapshot(daqJ, procJ, storJ, saveEnabled);
        {
            std::lock_guard<std::mutex> lock(g_snapMtx);
            g_snap = snap;
            g_snapUpdated = true;
            g_snapCv.notify_all();
        }
        for (int i = 0; i < 10 && !g_stop.load(); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    ipc.close();
    stopBackend();
    if (httpThread.joinable()) httpThread.join();
    std::cout << "[webui] shutdown\n";
    return 0;
}
