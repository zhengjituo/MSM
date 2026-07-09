#ifndef MSM_MSGATEWAY_H
#define MSM_MSGATEWAY_H

#include "Module.h"
#include "MessageBus.h"
#include "Message.h"
#include <zmq.h>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <thread>
#include <atomic>
#include <mutex>
#include <queue>

namespace msm {

class MSMGateway : public Module {
public:
    struct PeerConfig {
        std::string name;
        std::string repEndpoint;
    };

    struct SubBinding {
        std::string remoteVar;
        std::string localVar;
    };

    struct SubConfig {
        std::string name;
        std::string endpoint;
        std::vector<SubBinding> bindings;
    };

    MSMGateway();
    ~MSMGateway();

    bool loadConfig(const std::string& jsonPath);
    void addPeer(const std::string& name, const std::string& repEndpoint);
    std::string firstPeerName() const;
    int repPort() const { return repPort_; }
    int pubPort() const { return pubPort_; }
    int restPort() const { return restPort_; }

    void start() override;
    void stop() override;
    void handleState(const Message& msg) override;
    ModuleVars* getVars() override;

private:
    void runREP();
    void runDEALER(int idx);
    void runPUB();
    void runSUB(int idx);

    Message jsonToMessage(const nlohmann::json& j);
    nlohmann::json messageToJson(const Message& msg, const std::string& type = "");

    int repPort_;
    int pubPort_;
    int restPort_ = 8080;

    void* ctx_ = nullptr;
    void* repSock_ = nullptr;
    void* pubSock_ = nullptr;

    std::atomic<bool> running_{false};

    struct PeerRuntime {
        std::string name;
        void* dealerSock = nullptr;
        std::thread dealerThread;
    };

    std::vector<PeerConfig> peerConfigs_;
    std::vector<PeerRuntime> peerRuntimes_;
    std::mutex peerMtx_;

    std::vector<SubConfig> subConfigs_;
    struct SubRuntime {
        void* subSock = nullptr;
        std::thread subThread;
    };
    std::vector<SubRuntime> subRuntimes_;

    std::thread repThread_;
    std::thread pubThread_;

    std::queue<std::string> pubQueue_;
    std::mutex pubMtx_;

    struct GatewayVars : public ModuleVars {
        std::map<std::string, std::string> list() override { return {}; }
        void* get(const std::string&) override { return nullptr; }
        bool set(const std::string&, const std::string&) override { return false; }
    } vars_;
};

} // namespace msm

#endif
