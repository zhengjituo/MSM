#include "mod_config.h"
#include "MessageBus.h"
#include <iostream>
#include <sstream>

ConfigModule::ConfigModule()
    : Module("ConfigModule")
{
    idleDelayMs_ = 100;
    registerVars();
}

void ConfigModule::registerVars() {
    declareVar("frame_size", config_.frame_size);
    declareVar("auto_acq", config_.auto_acq_on_start);
}

msm::ModuleVars* ConfigModule::getVars() {
    return &tableVars_;
}

void ConfigModule::handleState(const msm::Message& msg) {
    if (msg.cmd == "LoadFromJson") {
        std::string path = msg.data.get("path");
        if (path.empty()) {
            path = msg.data.get("", ""); // maybe passed as unnamed arg
            if (path.empty()) {
                std::cerr << "[ConfigModule] LoadFromJson: no path\n";
                return;
            }
        }
        onLoadFromJson(path);

    } else if (msg.cmd == "LoadFromJsonString") {
        std::string jsonStr = msg.data.get("json");
        if (!jsonStr.empty())
            onLoadFromJsonString(jsonStr);

    } else if (msg.cmd == "SaveToJson") {
        std::string path = msg.data.get("path");
        if (path.empty()) {
            std::cerr << "[ConfigModule] SaveToJson: no path\n";
            return;
        }
        onSaveToJson(path);

    } else if (msg.cmd == "Distribute") {
        onDistribute(msg.data.getBool("isInit", false));

    } else if (msg.cmd == "DistributeTo") {
        onDistributeTo(msg.data.data);

    } else if (msg.cmd == "SetParam") {
        std::string kv = msg.data.get("value");
        if (!kv.empty()) onSetParam(kv);

    } else if (msg.cmd == "GetParam") {
        std::string path = msg.data.get("path");
        if (!path.empty()) onGetParam(path);

    } else if (msg.cmd == "Status") {
        msm::Message response("Response");
        response.setTarget(msg.getSender());
        response.setCorrelationId(msg.getCorrelationId());
        response.data.set("n_cards", (int)config_.cards.size());
        response.data.set("frame_size", (int)config_.frame_size);
        msm::MessageBus::instance().send(response);
    }
}

void ConfigModule::onLoadFromJson(const std::string& path) {
    if (config_.loadFromJson(path))
        std::cout << "[ConfigModule] loaded from " << path << "\n";
    else
        std::cerr << "[ConfigModule] load failed: " << path << "\n";
}

void ConfigModule::onLoadFromJsonString(const std::string& jsonStr) {
    if (config_.loadFromJsonString(jsonStr))
        std::cout << "[ConfigModule] loaded from string\n";
    else
        std::cerr << "[ConfigModule] load from string failed\n";
}

void ConfigModule::onSaveToJson(const std::string& path) const {
    config_.saveToJson(path);
}

void ConfigModule::onDistribute(bool isInit) {
    if (config_.cards.empty()) {
        std::cerr << "[ConfigModule] no config to distribute\n";
        return;
    }

    // Distribute to DAQModule
    {
        msm::Message daqMsg(isInit ? "Init" : "Configure");
        daqMsg.data.data = config_.toKeyValues();
        daqMsg.setTarget("DAQModule");
        daqMsg.setSender("ConfigModule");
        msm::MessageBus::instance().send(daqMsg);
    }

    // Distribute to Process
    {
        msm::Message procMsg("Configure");
        procMsg.setTarget("Process");
        procMsg.setSender("ConfigModule");
        procMsg.data.set("BatchSize", 10000);
        procMsg.data.set("FrameSize", (int)config_.frame_size);
        procMsg.data.set("StorageTarget", "Storage");
        msm::MessageBus::instance().send(procMsg);
    }

    // Distribute to Storage
    {
        msm::Message storMsg("Configure");
        storMsg.setTarget("Storage");
        storMsg.setSender("ConfigModule");
        storMsg.data.set("SavePath", config_.data_save_path);
        storMsg.data.set("FelName", config_.fel_name);
        storMsg.data.set("SystemName", config_.system_name);
        storMsg.data.set("DeviceName", config_.device_name);
        storMsg.data.set("FileType", config_.file_type);
        storMsg.data.set("MaxSaveSizeMB", config_.max_save_size_mb);
        storMsg.data.set("ExpNo", config_.exp_no);
        storMsg.data.set("RunSerial", config_.run_serial);
        storMsg.data.set("BatchSize", 10000);
        storMsg.data.set("FrameSize", (int)config_.frame_size);
        storMsg.data.set("Slot", config_.slot);
        msm::MessageBus::instance().send(storMsg);
    }

    std::cout << "[ConfigModule] distributed config"
              << (isInit ? " (Init)" : " (Configure)") << "\n";
}

void ConfigModule::onDistributeTo(std::map<std::string, std::string> kv) {
    std::string target;
    auto it = kv.find("target");
    if (it != kv.end()) {
        target = it->second;
        kv.erase(it);
    }

    msm::Message msg("Configure");
    msg.setTarget(target);
    msg.setSender("ConfigModule");
    msg.data.data = kv;
    msm::MessageBus::instance().send(msg);
}

void ConfigModule::onSetParam(const std::string& pathVal) {
    // format: "/path/to/param=value"
    size_t eq = pathVal.find('=');
    if (eq == std::string::npos) {
        std::cerr << "[ConfigModule] SetParam: need key=value, got " << pathVal << "\n";
        return;
    }
    std::string path = pathVal.substr(0, eq);
    std::string val = pathVal.substr(eq + 1);

    // Parse path and update config_
    // Format: /card<N>/ch<N>/field  or  /<app_field>
    if (path.substr(0, 6) == "/card_") {
        size_t next = path.find('/', 6);
        if (next == std::string::npos) return;
        int cardIdx = std::stoi(path.substr(6, next - 6)) - 1;
        if (cardIdx < 0 || cardIdx >= (int)config_.cards.size()) return;

        std::string rest = path.substr(next + 1);
        if (rest.substr(0, 3) == "ch_") {
            size_t sep = rest.find('/', 3);
            int chIdx = std::stoi(rest.substr(3, sep - 3)) - 1;
            if (chIdx < 0 || chIdx >= (int)config_.cards[cardIdx].channels.size()) return;
            std::string field = rest.substr(sep + 1);
            auto& ch = config_.cards[cardIdx].channels[chIdx];

            if (field == "trig_length") ch.trig_length = val;
            else if (field == "wave_data_enable") ch.wave_data_enable = val;
            else if (field == "trig_source") ch.trig_source = val;
            else if (field == "trig_mode") ch.trig_mode = val;
            else if (field == "trig_level") ch.trig_level = val;
            else if (field == "trig_pulse_width") ch.trig_pulse_width = val;
            else if (field == "trig_polarity") ch.trig_polarity = val;
            else if (field == "trig_delay_time") ch.trig_delay_time = val;
            else if (field == "trig_position") ch.trig_position = val;
            else if (field == "trig_window_start") ch.trig_window_start = val;
            else if (field == "voltage_range") ch.voltage_range = val;
            else if (field == "voltage_offset") ch.voltage_offset = val;
            else if (field == "dead_time") ch.dead_time = val;
            else if (field == "finite_trig_num") ch.finite_trig_num = val;
            else if (field == "work_mode") ch.work_mode = val;
            else if (field == "runEnable") ch.runEnable = (val == "1" || val == "true");
            else { std::cerr << "[ConfigModule] unknown field " << field << "\n"; return; }

            std::cout << "[ConfigModule] set " << path << " = " << val << "\n";
            return;
        }
    }

    // app_config fields
    if (path == "/data_save_path") config_.data_save_path = val;
    else if (path == "/fel_name") config_.fel_name = val;
    else if (path == "/system_name") config_.system_name = val;
    else if (path == "/device_name") config_.device_name = val;
    else if (path == "/run_serial") config_.run_serial = val;
    else if (path == "/exp_no") config_.exp_no = std::stoi(val);
    else if (path == "/max_save_size_mb") config_.max_save_size_mb = std::stoi(val);
    else if (path == "/trig_delay_time_fix") config_.trig_delay_time_fix = std::stoi(val);
    else if (path == "/auto_acq_on_start") config_.auto_acq_on_start = (val == "1" || val == "true");
    else {
        std::cerr << "[ConfigModule] unknown path " << path << "\n";
        return;
    }

    std::cout << "[ConfigModule] set " << path << " = " << val << "\n";
}

void ConfigModule::onGetParam(const std::string& path) {
    // For now, just print to stdout. Could send a response message.
    std::string val;
    if (path == "/frame_size") val = std::to_string(config_.frame_size);
    else if (path == "/data_save_path") val = config_.data_save_path;
    else if (path == "/fel_name") val = config_.fel_name;
    else if (path == "/system_name") val = config_.system_name;
    else if (path == "/device_name") val = config_.device_name;
    else if (path == "/n_cards") val = std::to_string(config_.cards.size());
    else val = "unknown";

    std::cout << "[ConfigModule] " << path << " = " << val << "\n";
}
