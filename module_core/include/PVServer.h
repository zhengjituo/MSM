#ifndef MSM_PV_SERVER_H
#define MSM_PV_SERVER_H

#include "Module.h"
#include "MessageBus.h"
#include "Message.h"
#include <nlohmann/json.hpp>
#include <map>
#include <vector>
#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <fstream>

namespace msm {

struct PVVarConfig {
    std::string name;
    std::string type;
    size_t offset = 0;
    size_t len = 0;
    size_t nelm = 0;
    std::string dir = "io";
    bool epics = true;
    std::string pvName;
};

struct PVModuleConfig {
    std::string name;
    size_t memorySize = 0;
    std::vector<PVVarConfig> vars;
};

struct PVVarRuntime {
    std::string moduleName;
    std::string varName;
    std::string type;
    std::string dir;
    size_t offset = 0;
    size_t length = 0;
    void* modPtr = nullptr;
    bool isArray = false;
    size_t nelm = 1;
};

struct PVModuleRuntime {
    std::string name;
    size_t memorySize = 0;
    void* basePtr = nullptr;
};

struct CAChannelContext {
    void* channel = nullptr;
    void* subscription = nullptr;
    void* subUser = nullptr;
    bool connected = false;
    bool disconnected = false;
    bool subscribed = false;
    long lastPushedInt = 0;
    double lastPushedDouble = 0;
    std::string pvName;
};

class PVServer : public Module {
public:
    PVServer(const std::string& pvPrefix = "MSM");
    ~PVServer();

    bool loadConfig(const std::string& jsonPath);
    void setModuleMemory(const std::string& moduleName, void* base, size_t size);

    void start() override;
    void stop() override;

    void handleState(const Message& msg) override;
    ModuleVars* getVars() override;

    bool generateDB(const std::string& filepath);
    bool snapshot(const std::string& filepath);
    bool restore(const std::string& filepath);

    void enableIOC(const std::string& epicsBase,
                   const std::string& dbPath = "msm_pvs.db");
    void onMonitorUpdate(const std::string& pvName, long dbrType, const void* data);

private:
    void writeMemory(void* ptr, const std::string& type, const std::string& value);
    std::string readMemory(void* ptr, const std::string& type);
    void pushToIOC(const std::string& pvName);
    void pushAllToIOC();
    void subscribeAll();
    bool caInitChannels();
    void caCleanup();

    pid_t spawnIOC();
    void killIOC();

    std::string prefix_;
    int snapshotInterval_ = 5;
    std::atomic<bool> stopped_{false};

    std::vector<PVModuleConfig> config_;
    std::vector<PVModuleRuntime> modules_;
    std::map<std::string, PVVarRuntime> varMap_;

    std::thread snapshotThread_;
    std::atomic<bool> snapshotRunning_{false};

    pid_t iocPID_ = -1;
    int iocStdin_ = -1;
    std::string iocEpicsBase_;
    std::string iocDBPath_;
    bool iocEnabled_ = false;

    std::map<std::string, CAChannelContext> caChannels_;

    struct PVServerVars : public ModuleVars {
        std::map<std::string, std::string> list() override { return {}; }
        void* get(const std::string&) override { return nullptr; }
        bool set(const std::string&, const std::string&) override { return false; }
    } vars_;
};

} // namespace msm

#endif
