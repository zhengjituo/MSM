#include "httplib.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/inotify.h>
#include <sys/poll.h>
#include <signal.h>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <fstream>
#include <sstream>
#include <cstring>
#include <unistd.h>

// nlohmann/json for parsing /tmp/msm_live.json
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
        unlink(path);
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

        // Read response (single JSON line)
        char buf[4096];
        int64_t total = 0;
        struct pollfd pfds[1] = {{fd_, POLLIN | POLLERR | POLLHUP, 0}};
        while (true) {
            int n = ::poll(pfds, 1, 2000);
            if (n <= 0) break;
            if (pfds[0].revents & (POLLERR | POLLHUP)) { ::close(fd_); fd_ = -1; break; }

            int r = ::recv(fd_, buf, sizeof(buf) - 1, 0);
            if (r <= 0) break;
            buf[r] = 0;
            total += r;
            if (strstr(buf, "\n")) break;
        }

        // Extract JSON from rbuf + received data
        std::string response;
        if (total > 0) {
            rbuf_.append(buf, total);
            size_t nl = rbuf_.find('\n');
            if (nl != std::string::npos) {
                response = rbuf_.substr(0, nl);
                rbuf_.erase(0, nl + 1);
            }
        }
        if (response.empty()) {
            response = R"({"status":"timeout"})";
            // Keep rbuf_ in case partial response arrives later
        }
        return response;
    }

    void close() { if (connected()) { ::close(fd_); fd_ = -1; } }
};

// ── 文件系统状态缓存 ───────────────────────────────────────────

struct AppState {
    bool saveEnabled = false;
    int  frameIndex = 0;
};
static AppState g_app;
static std::mutex g_appMux;

// /tmp/msm_live.json 解析和 SSE 推送
static std::mutex g_stateMtx;
static std::condition_variable g_stateCv;
static json g_liveState;
static bool g_stateUpdated = false;

static void watchLiveJson() {
    int idfd = inotify_init();
    int wd = inotify_add_watch(idfd, "/tmp/msm_live.json", IN_MODIFY);
    if (wd < 0) {
        std::cerr << "[webui] inotify watch failed, exiting\n";
        return;
    }

    struct pollfd pfd[1] = {{idfd, POLLIN, 0}};
    while (!g_stop.load()) {
        int n = poll(pfd, 1, 5000);
        if (n <= 0) continue;

        char buf[256];
        if (read(idfd, buf, sizeof(buf)) < 0) continue;

        std::ifstream f("/tmp/msm_live.json");
        if (!f.is_open()) continue;
        std::string content((std::istreambuf_iterator<char>(f)),
                            (std::istreambuf_iterator<char>()));

        try {
            json j = json::parse(content);
            std::lock_guard<std::mutex> lock(g_stateMtx);
            g_liveState = std::move(j);
            g_stateUpdated = true;
            g_stateCv.notify_all();
        } catch (...) {}
    }
    ::close(idfd);
}

static std::string buildSseData() {
    std::lock_guard<std::mutex> lock(g_stateMtx);
    auto& j = g_liveState;
    std::ostringstream o;
    o << "data: {";
    o << "\"save_enabled\":" << (g_app.saveEnabled ? "true" : "false") << ",";
    o << "\"frame_index\":" << g_app.frameIndex << ",";
    o << "\"running\":" << (j.value("running", false) ? "true" : "false") << ",";
    o << "\"channels\":[";

    if (j.is_object() && j.contains("channels") && j["channels"].is_array()) {
        for (size_t i = 0; i < j["channels"].size(); i++) {
            if (i > 0) o << ",";
            auto& ch = j["channels"][i];
            o << "{";
            o << "\"ch\":" << ch.value("ch", i + 1);
            o << ",\"daq_frames\":" << ch.value("frames", 0);
            o << ",\"processed\":" << ch.value("processed", 0);
            o << ",\"batches\":" << ch.value("batches", 0);
            o << ",\"saved\":" << ch.value("saved", 0);
            o << ",\"trig_id\":" << ch.value("trig_id", 0);
            o << ",\"bunch_id\":" << ch.value("bunch_id", 0);
            o << "}";
        }
    }
    o << "]}\n\n";
    return o.str();
}

// ── 主程序 ─────────────────────────────────────────────────────

int main() {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, [](int) { g_stop.store(true); });
    signal(SIGTERM, [](int) { g_stop.store(true); });

    // 1) Connect to V2 IPC socket
    IPCClient ipc;
    if (!ipc.connect("/tmp/msm_daq.sock")) {
        // Retry a few times
        for (int retry = 0; retry < 5; retry++) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (ipc.connect("/tmp/msm_daq.sock")) break;
        }
    }

    // 2) Start file watcher thread
    std::thread watchThread(watchLiveJson);
    watchThread.detach();

    // 3) HTTP server
    httplib::Server svr;

    // POST /api/msg — forward to IPC
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
            {
                std::lock_guard<std::mutex> lock(g_appMux);
                g_app.saveEnabled = true;
            }
            res.set_content(R"({"status":"save enabled"})", "application/json");
        } else if (cmd == "disablesave") {
            ipc.send("DISABLESAVE");
            {
                std::lock_guard<std::mutex> lock(g_appMux);
                g_app.saveEnabled = false;
            }
            res.set_content(R"({"status":"save disabled"})", "application/json");
        } else if (cmd == "reload") {
            ipc.send("RELOAD");
            res.set_content(R"({"status":"reloading"})", "application/json");
        } else if (cmd == "quit") {
            ipc.send("QUIT");
            res.set_content(R"({"status":"quitting"})", "application/json");
        } else {
            // Forward as MSG command
            std::string r = ipc.send("MSG " + cmd);
            res.set_content(r, "application/json");
        }
    });

    // GET /api/events — SSE
    svr.Get("/api/events", [&](const httplib::Request& req, httplib::Response& res) {
        res.set_chunked_content_provider("text/event-stream",
            [&](size_t, httplib::DataSink& sink) {
                std::unique_lock<std::mutex> lock(g_stateMtx);
                if (!g_stateUpdated) {
                    g_stateCv.wait_for(lock, std::chrono::milliseconds(500));
                    if (!g_stateUpdated) return g_stop.load() ? false : true;
                }
                std::string sse = buildSseData();
                sink.os << sse;
                sink.os.flush();
                g_stateUpdated = false;
                return !g_stop.load();
            });
    });

    // GET /api/status — initial load
    svr.Get("/api/status", [](const httplib::Request& req, httplib::Response& res) {
        res.set_content(R"({"running":false,"save_enabled":false,"frame_index":0,"channels":[]})",
                        "application/json");
    });

    // GET / — serve dashboard HTML from file
    svr.Get("/", [](const httplib::Request& req, httplib::Response& res) {
        std::ifstream f("web/msm_daq_hw.html");
        if (!f.is_open()) {
            res.status = 404;
            res.set_content("not found", "text/plain");
            return;
        }
res.set_content(std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()), "text/html");
    });

    // Start HTTP server in a thread
    std::thread httpThread([&] {
        std::cout << "[webui] http://localhost:8080\n";
        svr.listen("0.0.0.0", 8080);
    });

    // Wait for shutdown (event-driven)
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    ipc.close();
    httpThread.join();

    std::cout << "[webui] shutdown\n";
    return 0;
}