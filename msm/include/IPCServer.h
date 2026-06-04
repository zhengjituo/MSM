#ifndef MSM_IPC_SERVER_H
#define MSM_IPC_SERVER_H

#include "Module.h"
#include "MessageBus.h"
#include "Message.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <map>
#include <thread>
#include <atomic>
#include <mutex>
#include <fstream>

namespace msm {

class IPCServer : public Module {
public:
    IPCServer(const std::string& socketPath = "/tmp/msm.sock");
    ~IPCServer();

    bool loadConfig(const std::string& jsonPath);
    void setModuleMemory(const std::string& moduleName, void* base, size_t size);

    void start() override;
    void stop() override;
    void handleState(const Message& msg) override;
    ModuleVars* getVars() override;

private:
    void runServer();
    bool handleClient(int clientFd);
    std::string process(const std::string& line);

    void writeMemory(void* ptr, const std::string& type, const std::string& value);
    std::string readMemory(void* ptr, const std::string& type);

    std::string socketPath_;
    int serverFd_ = -1;
    std::atomic<bool> running_{false};
    std::thread serverThread_;

    struct VarConfig {
        std::string name;
        std::string type;
        size_t offset = 0;
        size_t len = 0;
        std::string dir = "io";
    };

    struct ModuleConfig {
        std::string name;
        size_t memorySize = 0;
        std::vector<VarConfig> vars;
    };

    struct VarRuntime {
        std::string type;
        std::string dir;
        size_t offset = 0;
        void* modPtr = nullptr;
    };

    std::vector<ModuleConfig> config_;
    std::map<std::string, VarRuntime> varMap_;

    struct IPCServerVars : public ModuleVars {
        std::map<std::string, std::string> list() override { return {}; }
        void* get(const std::string&) override { return nullptr; }
        bool set(const std::string&, const std::string&) override { return false; }
    } vars_;
};

} // namespace msm

#endif
