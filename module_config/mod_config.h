#ifndef MOD_CONFIG_H
#define MOD_CONFIG_H

#include "Module.h"
#include "config_impl.h"
#include <string>

class ConfigModule : public msm::Module {
public:
    ConfigModule();

    void handleState(const msm::Message& msg) override;
    msm::ModuleVars* getVars() override;

    ConfigImpl& config() { return config_; }
    const ConfigImpl& config() const { return config_; }

private:
    void onLoadFromJson(const std::string& path);
    void onLoadFromJsonString(const std::string& jsonStr);
    void onSaveToJson(const std::string& path) const;
    void onDistribute(bool isInit);
    void onDistributeTo(std::map<std::string, std::string> kv);
    void onSetParam(const std::string& pathVal);
    void onGetParam(const std::string& path);
    void registerVars();

    ConfigImpl config_;
};

#endif
