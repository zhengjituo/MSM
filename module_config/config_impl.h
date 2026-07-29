#ifndef CONFIG_IMPL_H
#define CONFIG_IMPL_H

#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <cstddef>

struct ChannelConfig {
    bool runEnable = true;
    int channel = 1;
    int cardIndex = 1;
    int slot = 5;

    std::string trig_source = "global";
    std::string trig_mode = "normal";
    std::string trig_length = "1024";
    std::string trig_level = "50";
    std::string trig_pulse_width = "16";
    std::string trig_polarity = "Rising";
    std::string trig_delay_time = "0";
    std::string trig_position = "178";
    std::string trig_window_start = "0";

    std::string voltage_range = "1000";
    std::string voltage_offset = "0";

    std::string dead_time = "0";
    std::string finite_trig_num = "0";
    std::string wave_data_enable = "1";
    std::string work_mode = "vpp";

    std::string k = "1";
    std::string k1st = "1";
    std::string kx = "1";
    std::string ky = "1";
    std::string offset_x = "1";
    std::string offset_y = "1";
    std::string b1st = "1";

    std::string value_threshold_high = "20";
    std::string value_threshold_low = "-20";
    std::string external_trig_polarity = "RisingE";
    std::string down_sample = "1GSa/s";

    std::string ip_global_rst = "0";
    std::string ip_logic_rst = "0";

    int GetFrameLength() const {
        if (wave_data_enable == "0" || wave_data_enable == "false") return 128;
        return std::stoi(trig_length) * 2 + 128;
    }

    size_t ring_buffer_size_mb = 64;
    size_t recv_buffer_size = 16 * 1024 * 1024;
    int read_timeout_ms = 10;
};

struct CardConfig {
    int card_index = 0;
    std::string card_type;
    int slot = 0;
    std::vector<ChannelConfig> channels;
};

struct ConfigImpl {
    int chassis_no = 1;
    std::vector<CardConfig> cards;

    bool auto_acq_on_start = false;
    std::string data_save_path = "/mnt/localssd";
    std::string file_name = "PD";
    std::string file_type = "dat";
    bool epics_control_mode = true;
    int max_save_size_mb = 1024;
    int pv_info_freq_hz = 2;
    int run_id = 234;
    bool start_storage = false;

    std::string fel_name = "fel1";
    std::string system_name = "HSS";
    std::string device_name = "PD";
    int exp_no = 1;
    std::string run_serial = "r00001";
    int slot = 0;
    size_t frame_size = 0;
    int trig_delay_time_fix = 1600;

    bool loadFromJson(const std::string& path);
    bool loadFromJsonString(const std::string& json);
    bool saveToJson(const std::string& path) const;
    std::map<std::string, std::string> toKeyValues() const;

    int findCardByType(const std::string& card_type) const;
    int findCardByIndex(int card_index) const;

    const ChannelConfig* getChannel(int card_index, int channel) const;
    ChannelConfig* getChannel(int card_index, int channel);
};

#endif
