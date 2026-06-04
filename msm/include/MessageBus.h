#ifndef MSM_MESSAGE_BUS_H
#define MSM_MESSAGE_BUS_H

#include "Message.h"
#include <map>
#include <string>
#include <memory>
#include <mutex>
#include <vector>
#include <utility>

namespace msm {

class Module;

class MessageBus {
public:
    static MessageBus& instance();
    
    void registerModule(Module* mod);
    void unregisterModule(const std::string& name);
    Module* findModule(const std::string& name);
    
    void send(const Message& msg);
    void subscribe(const std::string& api, const std::string& source, const std::string& subscriber);
    void unsubscribe(const std::string& api, const std::string& source, const std::string& subscriber);
    void broadcast(const Message& msg);
    
    void shutdown();
    
private:
    MessageBus() = default;
    ~MessageBus() = default;
    
    std::map<std::string, Module*> modules_;
    std::map<std::string, std::map<std::string, std::vector<std::string>>> subscriptions_;
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> bySubscriber_;
    std::mutex mtx_;
};

} // namespace msm

#endif