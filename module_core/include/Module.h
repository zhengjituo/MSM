#ifndef MSM_MODULE_H
#define MSM_MODULE_H

#include "Message.h"
#include <string>
#include <thread>
#include <map>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>
#include <vector>

namespace msm {

// ─── VarTable: generic named variable registry ─────────────────
// Module declareVar() at construction; no if-chains, no offsets.
// One declareVar per exposed variable; reader/writer lambdas capture member refs.
class VarTable {
public:
    // dir: "i"=writable, "o"=readonly, "io"=both
    void declare(const std::string& name, const std::string& type,
                 std::function<std::string()> reader,
                 std::function<bool(const std::string&)> writer = nullptr,
                 const std::string& dir = "o");

    std::string read(const std::string& name) const;
    bool write(const std::string& name, const std::string& value);
    bool has(const std::string& name) const;

    // name → current value string
    std::map<std::string, std::string> readAll() const;

    // name → type string
    std::map<std::string, std::string> allTypes() const;
    std::vector<std::string> names() const;

    // Export to msm_vars.json format (without offsets)
    // prefix: EPICS PV prefix (e.g. "MSM")
    // moduleName: this module's name
    std::string toJSON(const std::string& prefix, const std::string& moduleName) const;
    bool saveJSON(const std::string& filepath,
                  const std::string& prefix, const std::string& moduleName) const;

    // Template helpers
    template<typename T>
    void declareAtomic(const std::string& name, std::atomic<T>& var,
                       const std::string& dir = "o") {
        declare(name, typeName<T>(),
            [&var]() { return valToStr(var.load()); },
            [&var](const std::string& v) { var.store(strToVal<T>(v)); return true; },
            dir
        );
    }

    template<typename T>
    void declareScalar(const std::string& name, T& var,
                       const std::string& dir = "o") {
        declare(name, typeName<T>(),
            [&var]() { return valToStr(var); },
            [&var](const std::string& v) { var = strToVal<T>(v); return true; },
            dir
        );
    }

private:
    struct Entry {
        std::string type;
        std::string dir;
        std::function<std::string()> read;
        std::function<bool(const std::string&)> write;
    };
    std::map<std::string, Entry> entries_;

    template<typename T> static std::string typeName();
    template<typename T> static std::string valToStr(const T& v);
    template<typename T> static T strToVal(const std::string& s);
};

// ─── ModuleVars interface ──────────────────────────────────────
struct ModuleVars {
    virtual ~ModuleVars() {}
    virtual void* get(const std::string& name) = 0;
    virtual bool set(const std::string& name, const std::string& value) = 0;
    virtual std::map<std::string, std::string> list() = 0;
    virtual std::map<std::string, std::string> getTypes() { return {}; }
};

// Default ModuleVars backed by VarTable (for modules that use declareVar)
class TableVars : public ModuleVars {
public:
    TableVars(VarTable* tbl) : tbl_(tbl) {}
    void* get(const std::string& name) override { (void)name; return nullptr; }
    bool set(const std::string& name, const std::string& value) override { return tbl_->write(name, value); }
    std::map<std::string, std::string> list() override { return tbl_->readAll(); }
    std::map<std::string, std::string> getTypes() override { return tbl_->allTypes(); }
private:
    VarTable* tbl_;
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
    Message post(const std::string& niString);
    virtual ModuleVars* getVars() { return &tableVars_; }
    virtual void* getVarBase() { return nullptr; }
    virtual size_t getVarSize() { return 0; }
    virtual std::map<std::string, std::string> getVariables() {
        auto* v = getVars();
        return v ? v->list() : std::map<std::string, std::string>{};
    }
    virtual void handleState(const Message& message) = 0;
    virtual void onIdle() {} // idle state frame — called when no messages pending
    
protected:
    // Convenience: register a variable in the module's VarTable
    // Example: declareVar("ch1_frames", ch1Frames_);
    template<typename T>
    void declareVar(const std::string& name, std::atomic<T>& var) {
        vars_.declareAtomic(name, var);
    }
    template<typename T>
    void declareVar(const std::string& name, T& var) {
        vars_.declareScalar(name, var);
    }
    void declareVar(const std::string& name, const std::string& type,
                    std::function<std::string()> reader,
                    std::function<bool(const std::string&)> writer = nullptr) {
        vars_.declare(name, type, std::move(reader), std::move(writer));
    }

    // Export variables schema to msm_vars.json format
    // Call after all declareVar() calls (typically in main(), after module construction)
    void dumpVarsJSON(const std::string& filepath, const std::string& prefix = "MSM") {
        vars_.saveJSON(filepath, prefix, name_);
    }

    std::string name_;
    std::atomic<bool> running_{false};
    int idleDelayMs_ = 100; // cv_.wait_for() timeout; subclass sets own rate
    
    std::unique_ptr<std::thread> thread_;
    std::queue<Message> queue_;
    std::mutex mtx_;
    std::condition_variable cv_;
    
    VarTable vars_;        // registered variable table
    TableVars tableVars_{&vars_};  // default ModuleVars from table
    
    void runLoop();
};

} // namespace msm

#endif