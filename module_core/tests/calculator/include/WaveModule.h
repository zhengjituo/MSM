#ifndef WAVE_MODULE_H
#define WAVE_MODULE_H

#include "Module.h"
#include "MessageBus.h"
#include <map>
#include <string>
#include <vector>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <ctime>

#pragma pack(push, 1)
struct WaveModuleData {
    int size = 10;
    int action = 0;
    double wave[10] = {};
};
#pragma pack(pop)

namespace msm {

class WaveModule : public Module {
public:
    WaveModule() : Module("WaveModule") {
        vars_.d = &data_;
        static bool seeded = false;
        if (!seeded) { std::srand(std::time(nullptr)); seeded = true; }
    }

    void* getVarBase() override { return &data_; }
    size_t getVarSize() override { return sizeof(data_); }

    ModuleVars* getVars() override { return &vars_; }

    void handleState(const Message& msg) override {
        (void)msg;
        if (data_.action) {
            data_.action = 0;
            for (int i = 0; i < 10; i++) {
                data_.wave[i] = static_cast<double>(std::rand()) / RAND_MAX * 10.0;
            }
            {
                Message u("PV_UPDATE");
                u.setTarget("PVServer");
                u.data.set("module", "WaveModule");
                u.data.set("var", "size");
                MessageBus::instance().send(u);
            }
            {
                Message u("PV_UPDATE");
                u.setTarget("PVServer");
                u.data.set("module", "WaveModule");
                u.data.set("var", "wave");
                MessageBus::instance().send(u);
            }
            {
                Message u("PV_UPDATE");
                u.setTarget("PVServer");
                u.data.set("module", "WaveModule");
                u.data.set("var", "action");
                MessageBus::instance().send(u);
            }
        }
    }

private:
    struct WaveModuleVars : public ModuleVars {
        WaveModuleData* d = nullptr;

        void* get(const std::string& name) override {
            if (name == "size") return &d->size;
            if (name == "action") return &d->action;
            if (name == "wave") return d->wave;
            return nullptr;
        }

        bool set(const std::string& name, const std::string& value) override {
            if (name == "wave") return parseWave(value);
            if (name == "action") {
                try { d->action = std::stoi(value); return true; }
                catch (...) {}
            }
            return false;
        }

        bool parseWave(const std::string& value) {
            if (value.empty()) return false;
            std::string v = value;
            if (v.front() == '[') v = v.substr(1);
            if (v.back() == ']') v = v.substr(0, v.size() - 1);

            std::vector<double> tmp;
            std::stringstream ss(v);
            std::string item;
            while (std::getline(ss, item, ',')) {
                item.erase(0, item.find_first_not_of(" \t"));
                item.erase(item.find_last_not_of(" \t") + 1);
                if (item.empty()) continue;
                try { tmp.push_back(std::stod(item)); }
                catch (...) { return false; }
            }

            std::memset(d->wave, 0, sizeof(d->wave));
            for (size_t i = 0; i < tmp.size() && i < 10; i++) {
                d->wave[i] = tmp[i];
            }
            return true;
        }

        std::map<std::string, std::string> list() override {
            std::stringstream ss;
            ss << "[";
            for (int i = 0; i < 10; i++) {
                if (i > 0) ss << ",";
                ss << d->wave[i];
            }
            ss << "]";
            return {
                {"wave", ss.str()},
                {"size", std::to_string(d->size)},
                {"action", std::to_string(d->action)}
            };
        }

        std::map<std::string, std::string> getTypes() override {
            return {
                {"wave", "double[]"},
                {"size", "int"},
                {"action", "int"}
            };
        }
    } vars_;

    WaveModuleData data_;
};

} // namespace msm

#endif
