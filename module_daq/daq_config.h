#ifndef DAQ_CONFIG_H
#define DAQ_CONFIG_H

#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <cstddef>

// ============================================================
// 一个通道的完整配置参数（对应 JSON: DaqParameter.Cards.Card#N.Channels.Channel#N）
// ============================================================
struct ChannelConfig {
    bool runEnable = true;
    int channel = 1;
    int cardIndex = 1;
    int slot = 5;

    // 触发参数
    std::string trig_source = "global";
    std::string trig_mode = "normal";
    std::string trig_length = "1024";
    std::string trig_level = "50";
    std::string trig_pulse_width = "16";
    std::string trig_polarity = "Rising";
    std::string trig_delay_time = "0";
    std::string trig_position = "178";
    std::string trig_window_start = "0";

    // 量程
    std::string voltage_range = "1000";
    std::string voltage_offset = "0";

    // SDK DMA 相关参数
    std::string dead_time = "0";
    std::string finite_trig_num = "0";
    std::string wave_data_enable = "1";
    std::string work_mode = "vpp";

    // AD9910(DDS) 相关
    std::string k = "1";
    std::string k1st = "1";
    std::string kx = "1";
    std::string ky = "1";
    std::string offset_x = "1";
    std::string offset_y = "1";
    std::string b1st = "1";

    // 阈值
    std::string value_threshold_high = "20";
    std::string value_threshold_low = "-20";
    std::string external_trig_polarity = "RisingE";

    // FPGA 复位
    std::string ip_global_rst = "0";
    std::string ip_logic_rst = "0";

    // 帧长 = header(64) + (WaveDataEnable ? TrigLength*2 : 0) + trailer(64)
    int GetFrameLength() const {
        if (wave_data_enable == "0" || wave_data_enable == "false") return 128;
        return std::stoi(trig_length) * 2 + 128;
    }

    // ---------- 采集模块参数（不在 JSON 中，使用默认值） ----------
    size_t ring_buffer_size_mb = 64;
    size_t recv_buffer_size = 16 * 1024 * 1024;
    int read_timeout_ms = 10;
};

// ============================================================
// 一张板卡的配置参数（对应 JSON: DaqParameter.Cards.Card#N）
// ============================================================
struct CardConfig {
    int card_index = 0;
    std::string card_type;
    int slot = 0;
    std::vector<ChannelConfig> channels;
};

// ============================================================
// 全局配置（对应 JSON: DaqParameter）
// ============================================================
struct DaqConfig {
    int chassis_no = 1;
    std::vector<CardConfig> cards;

    // app_config 字段（仅数据存储相关）
    bool auto_acq_on_start = false;
    std::string data_save_path = "/mnt/localssd";
    std::string file_name = "PD";
    std::string file_type = "dat";
    bool epics_control_mode = true;
    int max_save_size_mb = 1024;
    int pv_info_freq_hz = 2;
    int run_id = 234;
    bool start_storage = false;

    // Storage V2 fields
    std::string fel_name = "fel1";
    std::string system_name = "HSS";
    std::string device_name = "PD";
    int exp_no = 1;
    std::string run_serial = "r00001";
    int slot = 0;
    size_t frame_size = 0;   // 由 loadFromJsonString 从 GetFrameLength() 自动推导
    int trig_delay_time_fix = 1600;

    bool loadFromJson(const std::string& path);
    bool loadFromJsonString(const std::string& json);
    bool saveToJson(const std::string& path) const;
    std::map<std::string, std::string> toKeyValues() const;

    // 查找指定类型板卡的索引（相对于 cards 数组）
    int findCardByType(const std::string& card_type) const;
    int findCardByIndex(int card_index) const;

    // 获取某张板卡上某个通道的配置（未找到返回 nullptr）
    const ChannelConfig* getChannel(int card_index, int channel) const;
    ChannelConfig* getChannel(int card_index, int channel);
};

#endif // DAQ_CONFIG_H
