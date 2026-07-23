#include "httplib.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/poll.h>
#include <signal.h>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <sstream>
#include <fstream>
#include <cstring>
#include <unistd.h>

#ifndef _WIN32
  #define JSON_USE_IMPLICIT_CONVERSIONS 0
  #define NLOHMANN_JSON_ABI_VERSION 3
#endif
#include "nlohmann/json.hpp"

using nlohmann::json;

static std::atomic<bool> g_stop{false};

// ── IPC 客户端 ──────────────────────────────────────────────────

class IPCClient {
    int fd_ = -1;
    std::mutex mux_;
    std::string rbuf_;
    bool connected() const { return fd_ >= 0; }
public:
    bool connect(const char* path) {
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
};
static AppSnapshot g_snap;
static std::mutex g_snapMtx;
static std::condition_variable g_snapCv;
static bool g_snapUpdated = false;

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
    return snap;
}

static std::string buildSseData(const AppSnapshot& snap) {
    std::ostringstream o;
    o << "data: {";
    o << "\"running\":" << (snap.running ? "true" : "false") << ",";
    o << "\"save_enabled\":" << (snap.saveEnabled ? "true" : "false");
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
    o << "]}\n\n";
    return o.str();
}

// ── 轮询线程 ────────────────────────────────────────────────────

static void pollIPC(IPCClient& ipc, bool& saveEnabled) {
    while (!g_stop.load()) {
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
}

// ── 主程序 ─────────────────────────────────────────────────────

int main() {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, [](int) { g_stop.store(true); });
    signal(SIGTERM, [](int) { g_stop.store(true); });

    IPCClient ipc;
    if (!ipc.connect("/tmp/msm_daq.sock")) {
        for (int retry = 0; retry < 5; retry++) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (ipc.connect("/tmp/msm_daq.sock")) break;
        }
    }

    bool saveEnabled = false;
    std::thread pollThread(pollIPC, std::ref(ipc), std::ref(saveEnabled));

    httplib::Server svr;

    svr.Post("/api/msg", [&](const httplib::Request& req, httplib::Response& res) {
        std::string cmd = req.body;
        if (cmd.empty()) {
            res.set_content(R"({"status":"empty"})", "application/json");
            return;
        }
        if (cmd == "start") {
            ipc.send("START");
            res.set_content(R"({"status":"started"})", "application/json");
        } else if (cmd == "stop") {
            ipc.send("STOP");
            res.set_content(R"({"status":"stopped"})", "application/json");
        } else if (cmd == "enablesave") {
            ipc.send("ENABLESAVE");
            saveEnabled = true;
            res.set_content(R"({"status":"save enabled"})", "application/json");
        } else if (cmd == "disablesave") {
            ipc.send("DISABLESAVE");
            saveEnabled = false;
            res.set_content(R"({"status":"save disabled"})", "application/json");
        } else if (cmd == "reload") {
            ipc.send("RELOAD");
            res.set_content(R"({"status":"reloading"})", "application/json");
        } else if (cmd == "quit") {
            ipc.send("QUIT");
            res.set_content(R"({"status":"quitting"})", "application/json");
        } else {
            std::string r = ipc.send("MSG " + cmd);
            res.set_content(r, "application/json");
        }
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

    while (!g_stop.load())
        std::this_thread::sleep_for(std::chrono::seconds(1));

    ipc.close();
    if (pollThread.joinable()) pollThread.join();
    httpThread.join();
    std::cout << "[webui] shutdown\n";
    return 0;
}
