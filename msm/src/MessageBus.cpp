#include "MessageBus.h"
#include "Module.h"
#include <iostream>
#include <algorithm>

namespace msm {

MessageBus& MessageBus::instance() {
    static MessageBus bus;
    return bus;
}

void MessageBus::registerModule(Module* mod) {
    std::lock_guard<std::mutex> lock(mtx_);
    modules_[mod->getName()] = mod;
}

void MessageBus::unregisterModule(const std::string& name) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = bySubscriber_.find(name);
        if (it != bySubscriber_.end()) {
            for (const auto& pair : it->second) {
                const std::string& api = pair.first;
                const std::string& source = pair.second;
                auto subIt = subscriptions_.find(api);
                if (subIt != subscriptions_.end()) {
                    auto& vec = subIt->second[source];
                    vec.erase(std::remove(vec.begin(), vec.end(), name), vec.end());
                }
            }
            bySubscriber_.erase(it);
        }
        modules_.erase(name);
    }
}

Module* MessageBus::findModule(const std::string& name) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = modules_.find(name);
    return (it != modules_.end()) ? it->second : nullptr;
}

void MessageBus::send(const Message& msg) {
    std::string target = msg.getTarget();
    
    if (target == "<broadcast>") {
        broadcast(msg);
        return;
    }
    
    if (target == "<register>") {
        subscribe(msg.cmd, msg.data.get("_source", "*"), msg.getSender());
        return;
    }
    
    if (target == "<unregister>") {
        unsubscribe(msg.cmd, msg.data.get("_source", "*"), msg.getSender());
        return;
    }
    
    if (msg.isResponse()) {
        Module* dst = findModule(target);
        if (dst) {
            dst->post(msg);
        }
        return;
    }
    
    Module* dst = findModule(target);
    if (dst) {
        dst->post(msg);
    }
}

void MessageBus::subscribe(const std::string& api, const std::string& source, const std::string& subscriber) {
    std::lock_guard<std::mutex> lock(mtx_);
    subscriptions_[api][source].push_back(subscriber);
    bySubscriber_[subscriber].push_back({api, source});
}

void MessageBus::unsubscribe(const std::string& api, const std::string& source, const std::string& subscriber) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = subscriptions_.find(api);
    if (it != subscriptions_.end()) {
        auto& vec = it->second[source];
        vec.erase(std::remove(vec.begin(), vec.end(), subscriber), vec.end());
    }
    auto it2 = bySubscriber_.find(subscriber);
    if (it2 != bySubscriber_.end()) {
        auto& list = it2->second;
        list.erase(std::remove_if(list.begin(), list.end(),
            [&api, &source](const std::pair<std::string, std::string>& p) {
                return p.first == api && p.second == source;
            }), list.end());
        if (list.empty()) {
            bySubscriber_.erase(it2);
        }
    }
}

void MessageBus::broadcast(const Message& msg) {
    std::string api = msg.cmd;
    std::string sender = msg.getSender();
    
    std::vector<std::string> targets;
    
    {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = subscriptions_.find(api);
        if (it != subscriptions_.end()) {
            if (it->second.count(sender)) {
                targets.insert(targets.end(), it->second[sender].begin(), it->second[sender].end());
            }
            if (it->second.count("*")) {
                targets.insert(targets.end(), it->second["*"].begin(), it->second["*"].end());
            }
        }
    }
    
    for (auto& t : targets) {
        Module* dst = findModule(t);
        if (dst) {
            dst->post(msg);
        }
    }
}

void MessageBus::shutdown() {
    std::lock_guard<std::mutex> lock(mtx_);
    modules_.clear();
    subscriptions_.clear();
    bySubscriber_.clear();
}

} // namespace msm