#ifndef ADD_MODULE_H
#define ADD_MODULE_H

#include "Module.h"
#include "MessageBus.h"
#include <map>
#include <string>

#pragma pack(push, 1)
struct AddModuleData {
    int a = 0;
    int b = 0;
    int sum = 0;
    int action = 0;
    int callRemote = 0;
    int remoteResult = 0;
};
#pragma pack(pop)

namespace msm {

class AddModule : public Module {
public:
    AddModule() : Module("AddModule"), remotePeer_("Peer") { vars_.d = &data_; }
    std::string remotePeer_;

    void* getVarBase() override { return &data_; }
    size_t getVarSize() override { return sizeof(data_); }

    ModuleVars* getVars() override { return &vars_; }

    void handleState(const Message& msg) override {
        if (msg.cmd == "Calculate") {
            int a = std::stoi(msg.data.get("a", "0"));
            int b = std::stoi(msg.data.get("b", "0"));
            data_.sum = a + b;
            Message resp("Response");
            resp.setTarget(msg.getSender());
            resp.setCorrelationId(msg.getCorrelationId());
            resp.data.set("sum", std::to_string(data_.sum));
            MessageBus::instance().send(resp);
            return;
        }
        if (data_.callRemote) {
            data_.callRemote = 0;
            Message req("Calculate");
            req.setTarget("MultiplyModule");
            req.setSender("AddModule");
            req.data.set("a", data_.a);
            req.data.set("b", data_.b);
            req.setAwaitResponse(true);
            req.setRemote(remotePeer_);
            Message resp = MessageBus::instance().sendAndWait(req, 5000);
            if (resp.cmd == "Response") {
                data_.remoteResult = std::stoi(resp.data.get("result", "0"));
            }
            {
                Message u("PV_UPDATE");
                u.setTarget("PVServer");
                u.data.set("module", "AddModule");
                u.data.set("var", "callRemote");
                MessageBus::instance().send(u);
            }
            {
                Message u("PV_UPDATE");
                u.setTarget("PVServer");
                u.data.set("module", "AddModule");
                u.data.set("var", "remoteResult");
                MessageBus::instance().send(u);
            }
            return;
        }
        if (data_.action) {
            data_.action = 0;
            data_.sum = data_.a + data_.b;
            {
                Message u("PV_UPDATE");
                u.setTarget("PVServer");
                u.data.set("module", "AddModule");
                u.data.set("var", "sum");
                MessageBus::instance().send(u);
            }
            {
                Message u("PV_UPDATE");
                u.setTarget("PVServer");
                u.data.set("module", "AddModule");
                u.data.set("var", "action");
                MessageBus::instance().send(u);
            }
            Message bc("PV_UPDATE");
            bc.setTarget("<broadcast>");
            bc.setSender("AddModule");
            bc.data.set("module", "AddModule");
            bc.data.set("var", "sum");
            bc.data.set("value", std::to_string(data_.sum));
            MessageBus::instance().send(bc);
        }
    }

private:
    struct AddModuleVars : public ModuleVars {
        AddModuleData* d = nullptr;

        void* get(const std::string& name) override {
            if (name == "a") return &d->a;
            if (name == "b") return &d->b;
            if (name == "sum") return &d->sum;
            if (name == "action") return &d->action;
            if (name == "callRemote") return &d->callRemote;
            if (name == "remoteResult") return &d->remoteResult;
            return nullptr;
        }

        bool set(const std::string& name, const std::string& value) override {
            try {
                int v = std::stoi(value);
                if (name == "a") { d->a = v; return true; }
                if (name == "b") { d->b = v; return true; }
                if (name == "action") { d->action = v; return true; }
                if (name == "callRemote") { d->callRemote = v; return true; }
                if (name == "remoteResult") { d->remoteResult = v; return true; }
            } catch (...) {}
            return false;
        }

        std::map<std::string, std::string> list() override {
            return {
                {"a", std::to_string(d->a)},
                {"b", std::to_string(d->b)},
                {"sum", std::to_string(d->sum)},
                {"action", std::to_string(d->action)},
                {"callRemote", std::to_string(d->callRemote)},
                {"remoteResult", std::to_string(d->remoteResult)}
            };
        }

        std::map<std::string, std::string> getTypes() override {
            return {
                {"a", "int"},
                {"b", "int"},
                {"sum", "int"},
                {"action", "int"},
                {"callRemote", "int"},
                {"remoteResult", "int"}
            };
        }
    } vars_;

    AddModuleData data_;
};

} // namespace msm

#endif
