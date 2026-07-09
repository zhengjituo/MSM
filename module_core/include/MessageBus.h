#ifndef MSM_MESSAGE_BUS_H
#define MSM_MESSAGE_BUS_H

#include "Message.h"
#include <map>
#include <string>
#include <memory>
#include <mutex>
#include <vector>
#include <utility>
#include <future>
#include <condition_variable>

namespace msm {

class Module;

class MessageBus {
public:
    static MessageBus& instance();
    
    void registerModule(Module* mod);
    void unregisterModule(const std::string& name);
    Module* findModule(const std::string& name);
    
    void send(const Message& msg);
    Message sendAndWait(Message& msg, int timeout_ms = 5000);
    void subscribe(const std::string& api, const std::string& source, const std::string& subscriber);
    void unsubscribe(const std::string& api, const std::string& source, const std::string& subscriber);
    void broadcast(const Message& msg);
    
    void shutdown();
    
private:
    MessageBus() = default;
    ~MessageBus() = default;
    
    void notifyResponse(const Message& msg);
    
    std::map<std::string, Module*> modules_;
    std::map<std::string, std::map<std::string, std::vector<std::string>>> subscriptions_;
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> bySubscriber_;
    std::mutex mtx_;
    
    std::map<uint64_t, std::promise<Message>> pendingResponses_;
    std::mutex pendingMtx_;
};

} // namespace msm

#endif