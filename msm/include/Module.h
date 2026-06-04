#ifndef MSM_MODULE_H
#define MSM_MODULE_H

#include "Message.h"
#include <string>
#include <thread>
#include <map>
#include <queue>
#include <mutex>
#include <atomic>

namespace msm {

struct ModuleVars {
    virtual ~ModuleVars() {}
    virtual void* get(const std::string& name) = 0;
    virtual bool set(const std::string& name, const std::string& value) = 0;
    virtual std::map<std::string, std::string> list() = 0;
    virtual std::map<std::string, std::string> getTypes() { return {}; }
};

class Module {
public:
    explicit Module(const std::string& name);
    virtual ~Module();
    
    const std::string& getName() const { return name_; }
    virtual void start();
    virtual void stop();
    bool isRunning() const { return running_.load(); }
    
    void post(const Message& msg);
    virtual ModuleVars* getVars() = 0;
    virtual void* getVarBase() { return nullptr; }
    virtual size_t getVarSize() { return 0; }
    virtual std::map<std::string, std::string> getVariables() {
        auto* v = getVars();
        return v ? v->list() : std::map<std::string, std::string>{};
    }
    virtual void handleState(const Message& message) = 0;
    
    virtual void onIdle() {}

protected:
    std::string name_;
    std::atomic<bool> running_{false};
    
    std::unique_ptr<std::thread> thread_;
    std::queue<Message> queue_;
    std::mutex mtx_;

    int idleDelayMs_ = 10;
    
    void runLoop();
};

} // namespace msm

#endif