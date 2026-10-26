#include "MessageBus.h"
#include "NIStringParser.h"
#include "IPCServer.h"
#include "mod_config.h"
#include "module_simsource.h"
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
#include <iomanip>
#include <mutex>
#include <string>

#include "config_handoff.h"

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

class ControllerModule : public msm::Module {
public:
    ControllerModule() : Module("Controller") {
        vars_.declare("last_start_error", "string",
                      []() { return getStartError(); }, nullptr, "o");
    }
    void handleState(const msm::Message& msg) override {
        if (msg.cmd == "Quit") {
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

static std::string g_initConfigPath = std::string(MSM_PROJECT_ROOT) + "/config/app_initial_config.json";
static std::string g_pendingConfigPath = std::string(MSM_PROJECT_ROOT) + "/config/app_pending_config.json";
static std::string g_runtimeConfigPath = std::string(MSM_PROJECT_ROOT) + "/config/app_runtime_config.json";

static void printStatus(const ModuleSim& sim,
                        const ModuleProcess& process,
                        const ModuleStorage& storage) {
    ModuleSim::Stats s = sim.getStats();
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
    configModule.config().slot = 0; // simsrc: no real hardware
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
    std::vector<UserCircularBuffer*> simRings;
    for (int i = 0; i < nCh; i++)
        simRings.push_back(new UserCircularBuffer(ringBytes, frameSize));

    ModuleSim sim;
    sim.setRings(simRings);
    bus.registerModule(&sim);
    sim.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    {
        int trigLen = 512;
        std::string waveData = "1";
        const auto* ch1 = cfg.getChannel(1, 1);
        if (ch1) {
            trigLen = std::stoi(ch1->trig_length);
            waveData = ch1->wave_data_enable;
        }

        msm::Message simCfg("Configure");
        simCfg.setTarget("DAQModule");
        simCfg.setSender("Main");
        simCfg.data.set("NumChannels", nCh);
        simCfg.data.set("TrigLength", trigLen);
        simCfg.data.set("WaveDataEnable", waveData);
        simCfg.data.set("FramesPerCycle", 10000);
        simCfg.data.set("TriggerRateHz", 100000);
        msm::MessageBus::instance().send(simCfg);
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));

    std::vector<UserCircularBuffer*> allRings;
    for (int i = 0; i < sim.getRingCount(); i++)
        allRings.push_back(sim.getRing(i));

    auto controller = std::make_shared<ControllerModule>();
    bus.registerModule(controller.get());
    controller->start();

    // start 完整链：每次 start 重载配置（pending→runtime）→ Configure 模拟源 → 开始采集
    g_startFn = [&]() {
        if (g_stop.load()) return;
        std::cout << "[main] start: reload config -> configure sim -> start\n";
        if (ensureRuntimeConfig(g_initConfigPath, g_pendingConfigPath, g_runtimeConfigPath) != 0) {
            failStart("配置交接失败（pending → runtime）");
            return;
        }
        if (!configModule.config().loadFromJson(g_runtimeConfigPath)) {
            failStart("配置加载失败: " + g_runtimeConfigPath);
            return;
        }
        const auto& cfg2 = configModule.config();
        int trigLen = 512;
        std::string waveData = "1";
        const auto* ch1 = cfg2.getChannel(1, 1);
        if (ch1) {
            trigLen = std::stoi(ch1->trig_length);
            waveData = ch1->wave_data_enable;
        }
        msm::Message simCfg("Configure");
        simCfg.setTarget("DAQModule");
        simCfg.setSender("Main");
        simCfg.data.set("NumChannels", sim.getRingCount());
        simCfg.data.set("TrigLength", trigLen);
        simCfg.data.set("WaveDataEnable", waveData);
        simCfg.data.set("FramesPerCycle", 10000);
        simCfg.data.set("TriggerRateHz", 100000);
        msm::Message resp = msm::MessageBus::instance().sendAndWait(simCfg, 5000);
        if (resp.cmd == "Timeout") {
            failStart("模拟源配置超时（5s）");
            return;
        }
        if (resp.data.get("status") != "ok") {
            std::string e = resp.data.get("error");
            failStart(e.empty() ? "模拟源配置失败" : e);
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

    UserCircularBuffer saveRing(256 * 1024 * 1024, 4096);

    std::vector<int> physCh;
    for (int i = 0; i < nCh; i++)
        physCh.push_back(i + 1);

    ModuleProcess process("Process", allRings, &saveRing, physCh);
    ModuleStorage storage(&saveRing, physCh);
    bus.registerModule(&process);
    bus.registerModule(&storage);
    process.start();
    storage.start();

    // Distribute config to all modules via ConfigModule
    {
        msm::Message dist("Distribute");
        dist.setTarget("ConfigModule");
        dist.setSender("Main");
        dist.data.set("isInit", false);
        msm::MessageBus::instance().send(dist);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::cout << "[main] SIM ready: " << sim.getRingCount() << " rings\n";
    std::cout << "[main] Running. Commands: start / stop / enablesave / disablesave / status / quit\n";

    std::thread cli([&]() {
        std::string line;
        while (!g_stop.load() && std::getline(std::cin, line)) {
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
                printStatus(sim, process, storage);
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
                printStatus(sim, process, storage);
                continue;
            }
            std::cout << "[main] Sending: " << line << std::endl;
            send(line);
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
    sim.stop();
    ipc.stop();
    configModule.stop();

    std::cin.setstate(std::ios::eofbit);
    close(STDIN_FILENO);

    bus.shutdown();

    if (cli.joinable())
        cli.join();
    return 0;
}
