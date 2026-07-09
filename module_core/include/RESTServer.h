#ifndef MSM_REST_SERVER_H
#define MSM_REST_SERVER_H

#include "Module.h"
#include "MessageBus.h"
#include "Message.h"
#include <nlohmann/json.hpp>
#include <httplib.h>
#include <string>
#include <vector>
#include <map>
#include <thread>
#include <atomic>
#include <mutex>
#include <fstream>

namespace msm {

class RESTServer : public Module {
public:
    RESTServer(int port = 8080);
    ~RESTServer();

    bool loadConfig(const std::string& jsonPath);
    void setModuleMemory(const std::string& moduleName, void* base, size_t size);

    void start() override;
    void stop() override;
    void handleState(const Message& msg) override;
    ModuleVars* getVars() override;
    void enableStaticDir(const std::string& dir);

private:
    void writeMemory(void* ptr, const std::string& type, const std::string& value);
    std::string readMemory(void* ptr, const std::string& type);
    std::string jsonResponse(bool ok, const std::string& value);
    std::string jsonError(const std::string& msg);

    int port_;
    std::string staticDir_;
    httplib::Server svr_;
    std::atomic<bool> httpRunning_{false};
    std::thread httpThread_;

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
        size_t length = 0;
        size_t nelm = 0;
        void* modPtr = nullptr;
    };

    std::vector<ModuleConfig> config_;
    std::map<std::string, VarRuntime> varMap_;

    struct RESTServerVars : public ModuleVars {
        std::map<std::string, std::string> list() override { return {}; }
        void* get(const std::string&) override { return nullptr; }
        bool set(const std::string&, const std::string&) override { return false; }
    } vars_;
};

} // namespace msm

#endif
