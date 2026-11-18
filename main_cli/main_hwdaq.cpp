#include "MessageBus.h"
#include "NIStringParser.h"
#include "IPCServer.h"
#include "mod_config.h"
#include "module_daq.h"
#include "module_process.h"
#include "module_storage.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <functional>
#include <csignal>
#include <atomic>
#include <memory>
#include <fstream>
#include <sstream>
#include <cmath>
#include <iomanip>
#include <poll.h>
#include <unistd.h>
#include <mutex>
#include <string>

static std::atomic<bool> g_stop{false};
static std::function<void()> g_startFn;
static void handle_signal(int) {
    g_stop.store(true);
}

// 最近一次 Start 失败原因（供状态查询/UI 展示）
static std::mutex g_startErrMtx;
static std::string g_lastStartError;
static std::string getStartError() {
    std::lock_guard<std::mutex> lk(g_startErrMtx);
    return g_lastStartError;
}
static void failStart(const std::string& err) {
    { std::lock_guard<std::mutex> lk(g_startErrMtx); g_lastStartError = err; }
    std::cerr << "[main] start FAILED: " << err << "\n";
}
static void clearStartError() {
    std::lock_guard<std::mutex> lk(g_startErrMtx);
    g_lastStartError.clear();
}

static void send(const std::string& niString) {
    msm::Message msg = msm::NIStringParser::parse(niString, "Main");
    msm::MessageBus::instance().send(msg);
}

static int countEnabledChannels(const ConfigImpl& cfg) {
    int n = 0;
    for (const auto& card : cfg.cards)
        for (const auto& ch : card.channels)
            if (ch.runEnable) n++;
    return n;
}

// 点数上限（总带宽 ~800MB/s 约束：下游还有网络传输需求）：
//   每卡启用通道 ≥3 → 点数 ≤512（8ch 全开：8×1152B×100k ≈ 922MB/s）
//   每卡启用通道 ≤2 → 点数 ≤1024（4ch：4×2176B×100k ≈ 870MB/s）
// 依据：每卡单读线程串行 4 通道，通道访问周期 ≈ SDK 缓冲灌满时间，
//       1024pt×每卡 4ch 贴极限运行（50s 实测丢 20-26%）；
//       每卡 ≤2ch 时通道周期减半，落在读线程能力内
static std::string checkTrigLengthCap(const ConfigImpl& cfg) {
    for (const auto& card : cfg.cards) {
        if (card.card_type != "1073") continue;
        int nEn = 0, maxPts = 0;
        for (const auto& ch : card.channels) {
            if (!ch.runEnable) continue;
            nEn++;
            int pts = 0;
            try { pts = std::stoi(ch.trig_length); } catch (...) {}
            if (pts > maxPts) maxPts = pts;
        }
        if (nEn == 0) continue;
        if (maxPts > 1024) {
            return "卡 slot=" + std::to_string(card.slot) + " 点数 " + std::to_string(maxPts) +
                   " 超过绝对上限 1024";
        }
        if (nEn >= 3 && maxPts > 512) {
            return "卡 slot=" + std::to_string(card.slot) + " 启用 " + std::to_string(nEn) +
                   " 通道（≥3）时点数上限 512，当前 " + std::to_string(maxPts) +
                   "。请降为 512，或将该卡启用通道减至 ≤2（总带宽约束 ~800MB/s，含网络传输）";
        }
    }
    return "";
}

class ControllerModule : public msm::Module {
public:
    ControllerModule() : Module("Controller") {
        vars_.declare("last_start_error", "string",
                      []() { return getStartError(); }, nullptr, "o");
    }
    void handleState(const msm::Message& msg) override {
        if (msg.cmd == "Quit") {
            std::cout << "[Controller] Quit triggered via IPC\n";
            g_stop.store(true);
        } else if (msg.cmd == "Start" && g_startFn) {
            g_startFn();
        } else if (msg.cmd == "Status") {
            msm::Message response("Response");
            response.setTarget(msg.getSender());
            response.setCorrelationId(msg.getCorrelationId());
            response.data.set("last_start_error", getStartError());
            msm::MessageBus::instance().send(response);
        }
    }
    msm::ModuleVars* getVars() override { return &tableVars_; }
};

#include "config_handoff.h"

static std::string g_initConfigPath = std::string(MSM_PROJECT_ROOT) + "/config/app_initial_config.json";
static std::string g_pendingConfigPath = std::string(MSM_PROJECT_ROOT) + "/config/app_pending_config.json";
static std::string g_runtimeConfigPath = std::string(MSM_PROJECT_ROOT) + "/config/app_runtime_config.json";

static void printStatus(const ModuleDAQ& daq,
                        const ModuleProcess& process,
                        const ModuleStorage& storage) {
    ModuleDAQ::Stats s = daq.getStats();
    std::cout << "\n======= Frame Summary =======\n";
    std::cout << "  "
              << std::setw(4) << "ch"
              << std::setw(11) << "DAQ"
              << std::setw(11) << "Process"
              << std::setw(11) << "Storage" << "\n";
    for (int i = 0; i < (int)s.total_frames.size(); i++) {
        std::cout << "  "
                  << std::setw(4) << ("ch" + std::to_string(i + 1))
                  << std::setw(11) << s.total_frames[i]
                  << std::setw(11) << process.getProcessedFrames(i)
                  << std::setw(11) << storage.getConsumedFrames(i)
                  << "\n";
    }
    std::cout << "============================\n" << std::endl;
}

int main() {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    auto& bus = msm::MessageBus::instance();

    // ConfigModule — single source of all configuration
    ConfigModule configModule;
    bus.registerModule(&configModule);
    configModule.start();

    // Load config: pending（待使用）→ runtime（运行使用），后端自主交接
    if (ensureRuntimeConfig(g_initConfigPath, g_pendingConfigPath, g_runtimeConfigPath) != 0) {
        std::cerr << "[main] Failed to ensure runtime config\n";
        return 1;
    }
    if (!configModule.config().loadFromJson(g_runtimeConfigPath)) {
        std::cerr << "[main] Failed to load " << g_runtimeConfigPath << "\n";
        return 1;
    }
    std::cout << "[main] Config loaded\n";

    const auto& cfg = configModule.config();

    int nCh = 0;
    for (const auto& card : cfg.cards)
        for (const auto& ch : card.channels)
            if (ch.runEnable) nCh++;
    if (nCh == 0) nCh = 4;

    size_t frameSize = cfg.frame_size;
    if (frameSize == 0) frameSize = 1152;
    const size_t ringBytes = cfg.ring_size_mb * 1024U * 1024U;
    std::cout << "[main] rings: " << nCh << " x " << cfg.ring_size_mb << "MB"
              << " (RingSizeMB)\n";
    std::vector<UserCircularBuffer*> daqRings;
    for (int i = 0; i < nCh; i++)
        daqRings.push_back(new UserCircularBuffer(ringBytes, frameSize));

    ModuleDAQ daq;
    daq.setRings(daqRings);
    bus.registerModule(&daq);
    daq.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    auto controller = std::make_shared<ControllerModule>();
    bus.registerModule(controller.get());
    controller->start();

    // start 完整链：每次 start 重载配置（pending→runtime）→ Init 重配硬件 → 开始采集
    g_startFn = [&]() {
        if (g_stop.load()) return;
        std::cout << "[main] start: reload config -> init hw -> start\n";
        if (ensureRuntimeConfig(g_initConfigPath, g_pendingConfigPath, g_runtimeConfigPath) != 0) {
            failStart("配置交接失败（pending → runtime）");
            return;
        }
        if (!configModule.config().loadFromJson(g_runtimeConfigPath)) {
            failStart("配置加载失败: " + g_runtimeConfigPath);
            return;
        }
        const auto& cfg2 = configModule.config();
        {
            const std::string capErr = checkTrigLengthCap(cfg2);
            if (!capErr.empty()) {
                failStart("点数限制: " + capErr);
                return;
            }
        }
        int nChNew = countEnabledChannels(cfg2);
        if (nChNew != daq.getRingCount()) {
            failStart("启用通道数变化（" + std::to_string(daq.getRingCount()) + "→" +
                      std::to_string(nChNew) +
                      "）：管线通道数固定，卡集合变更后请重启进程");
            return;
        }
        msm::Message daqInit("Init");
        daqInit.data.data = cfg2.toKeyValues();
        daqInit.setTarget("DAQModule");
        daqInit.setSender("Main");
        msm::Message resp = msm::MessageBus::instance().sendAndWait(daqInit, 60000);
        if (resp.cmd == "Timeout") {
            failStart("硬件初始化超时（60s）：检查机箱连接与配置 slot");
            return;
        }
        if (resp.data.get("status") != "ok") {
            std::string e = resp.data.get("error");
            failStart(e.empty() ? "硬件初始化失败（未返回原因）" : e);
            return;
        }
        clearStartError();
        // 保存开关（app_config.startStorage）：Storage/Process 同时收，本次 start 生效
        send(cfg2.start_storage ? "EnableSave >> -@ Storage" : "DisableSave >> -@ Storage");
        send(cfg2.start_storage ? "EnableSave >> -@ Process" : "DisableSave >> -@ Process");
        send("Start >> -@ Storage");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        send("Start >> -@ Process");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        send("Start >> -@ DAQModule");
    };

    msm::IPCServer ipc("/tmp/msm_daq.sock");
    bus.registerModule(&ipc);
    ipc.start();

    // First-time Init: send to DAQModule (triggers onReadConfig + initHW + updateConfig)
    {
        msm::Message daqInit("Init");
        daqInit.data.data = cfg.toKeyValues();
        daqInit.setTarget("DAQModule");
        daqInit.setSender("Main");
        msm::MessageBus::instance().send(daqInit);
    }
    while (!daq.isConfigured())
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    std::vector<UserCircularBuffer*> allRings = daqRings;

    UserCircularBuffer saveRing(256 * 1024 * 1024, 4096);

    std::vector<int> physCh;
    for (int i = 0; i < daq.getRingCount(); i++)
        physCh.push_back(i + 1);

    ModuleProcess process("Process", allRings, &saveRing, physCh);
    ModuleStorage storage(&saveRing, physCh);
    bus.registerModule(&process);
    bus.registerModule(&storage);
    process.start();
    storage.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Configure Process/Storage via ConfigModule distribute
    {
        msm::Message dist("Distribute");
        dist.setTarget("ConfigModule");
        dist.setSender("Main");
        dist.data.set("isInit", false);
        msm::MessageBus::instance().send(dist);
    }

    // 等首次 Init 完成（成功或失败）；失败不死等 —— 进程保持存活，
    // 用户修正配置后 start 会重新 initHW
    for (int i = 0; i < 600 && !daq.isHwReady(); i++) {
        if (daq.isInitFailed()) {
            std::cerr << "[main] DAQ Init 失败: " << daq.initError()
                      << "（进程保持存活，修正配置后可 start 重试）\n";
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "[main] DAQ ready: " << daq.getRingCount() << " rings\n";

    std::cout << "[main] Running. IPC: /tmp/msm_daq.sock\n"
              << "Commands: start / stop / enablesave / disablesave / status / quit\n";

    std::thread cli([&]() {
        std::string rbuf;
        while (!g_stop.load()) {
            struct pollfd pfd;
            pfd.fd = STDIN_FILENO;
            pfd.events = POLLIN;
            int rc = poll(&pfd, 1, 200);
            if (rc < 0) { if (errno == EINTR) continue; break; }
            if (rc == 0) continue;
            if (!(pfd.revents & POLLIN)) continue;
            char buf[4096];
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0) break;
            for (ssize_t i = 0; i < n; i++) {
                char c = buf[i];
                if (c == '\n') {
                    std::string line = rbuf;
                    rbuf.clear();
                    if (line.empty()) continue;
                    if (line == "quit" || line == "exit") {
                        g_stop.store(true);
                        break;
                    }
                    if (line == "start") {
                        if (g_startFn) g_startFn();
                        continue;
                    }
                    if (line == "stop") {
                        send("Stop >> -@ DAQModule");
                        std::this_thread::sleep_for(std::chrono::milliseconds(500));
                        send("Stop >> -@ Process");
                        std::this_thread::sleep_for(std::chrono::milliseconds(500));
                        send("Stop >> -@ Storage");
                        for (int i = 0; i < 50 && storage.isFlushing(); i++)
                            std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        continue;
                    }
                    if (line == "enablesave") {
                        send("EnableSave >> -@ Storage");
                        send("EnableSave >> -@ Process");
                        continue;
                    }
                    if (line == "disablesave") {
                        send("DisableSave >> -@ Storage");
                        send("DisableSave >> -@ Process");
                        continue;
                    }
                    if (line == "status") {
                        printStatus(daq, process, storage);
                        continue;
                    }
                    std::cout << "[main] Sending: " << line << std::endl;
                    send(line);
                } else if (c != '\r') {
                    rbuf += c;
                }
            }
        }
    });

    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cout << "\n[main] Shutting down...\n";
    send("Stop >> -@ DAQModule");
    send("Stop >> -@ Process");
    send("Stop >> -@ Storage");
    for (int i = 0; i < 50 && storage.isFlushing(); i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    process.stop();
    storage.stop();
    daq.stop();
    ipc.stop();
    configModule.stop();

    std::cin.setstate(std::ios::eofbit);
    close(STDIN_FILENO);

    std::thread watchdog([] {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        std::cerr << "[main] FORCE EXIT after 5s timeout\n";
        _exit(1);
    });
    watchdog.detach();

    bus.shutdown();

    if (cli.joinable())
        cli.join();
    std::cout << "[main] exited cleanly.\n";
    return 0;
}
