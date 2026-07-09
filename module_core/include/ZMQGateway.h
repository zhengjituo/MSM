#ifndef MSM_ZMQ_GATEWAY_H
#define MSM_ZMQ_GATEWAY_H

#include "Module.h"
#include "MessageBus.h"
#include "Message.h"
#include <zmq.h>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <map>
#include <thread>
#include <atomic>
#include <mutex>

namespace msm {

class ZMQGateway : public Module {
public:
    enum Mode { Server, Client };

    ZMQGateway(const std::string& endpoint, Mode mode = Server);
    ~ZMQGateway();

    void start() override;
    void stop() override;
    void handleState(const Message& msg) override;
    ModuleVars* getVars() override;

    void setPeerModules(const std::vector<std::string>& modules);

private:
    void runServer();
    void runClient();
    void processRemoteMessage(const std::string& identity, const nlohmann::json& j);
    void forwardToRemote(const Message& msg);
    void sendDiscovery();

    std::string endpoint_;
    Mode mode_;
    void* ctx_ = nullptr;
    void* sock_ = nullptr;
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::mutex mtx_;

    std::vector<std::string> peerModules_;

    struct GatewayVars : public ModuleVars {
        std::map<std::string, std::string> list() override { return {}; }
        void* get(const std::string&) override { return nullptr; }
        bool set(const std::string&, const std::string&) override { return false; }
    } vars_;
};

} // namespace msm

#endif
