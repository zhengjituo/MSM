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

std::vector<Module*> MessageBus::modules() {
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<Module*> out;
    out.reserve(modules_.size());
    for (auto& [name, mod] : modules_)
        if (mod) out.push_back(mod);
    return out;
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
        notifyResponse(msg);
        return;
    }
    
    if (msg.isRemote()) {
        Module* gw = findModule("MSMGateway");
        if (gw) {
            gw->post(msg);
        }
        return;
    }
    
    Module* dst = findModule(target);
    if (dst) {
        dst->post(msg);
    }
}

void MessageBus::notifyResponse(const Message& msg) {
    uint64_t cid = msg.getCorrelationId();
    std::lock_guard<std::mutex> lock(pendingMtx_);
    auto it = pendingResponses_.find(cid);
    if (it != pendingResponses_.end()) {
        try {
            it->second.set_value(msg);
        } catch (...) {}
        pendingResponses_.erase(it);
    }
}

Message MessageBus::sendAndWait(Message& msg, int timeout_ms) {
    if (msg.getCorrelationId() == 0) {
        msg.setCorrelationId(Message::generateCorrelationId());
    }
    msg.setAwaitResponse(true);

    std::promise<Message> promise;
    std::future<Message> future = promise.get_future();

    uint64_t cid = msg.getCorrelationId();
    {
        std::lock_guard<std::mutex> lock(pendingMtx_);
        pendingResponses_[cid] = std::move(promise);
    }

    send(msg);

    if (future.wait_for(std::chrono::milliseconds(timeout_ms)) == std::future_status::ready) {
        std::lock_guard<std::mutex> lock(pendingMtx_);
        pendingResponses_.erase(cid);
        return future.get();
    }

    std::lock_guard<std::mutex> lock(pendingMtx_);
    pendingResponses_.erase(cid);
    Message t;
    t.cmd = "Timeout";
    return t;
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

    Module* gw = findModule("MSMGateway");
    if (gw) gw->post(msg);
}

void MessageBus::shutdown() {
    std::lock_guard<std::mutex> lock(mtx_);
    modules_.clear();
    subscriptions_.clear();
    bySubscriber_.clear();
}

} // namespace msm