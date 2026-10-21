#ifndef MSM_DAQ_MODULE_DAQ_H
#define MSM_DAQ_MODULE_DAQ_H

#include "Module.h"
#include "user_circular_buffer.h"
#include "daq_config.h"
#include <atomic>
#include <thread>
#include <cstdint>
#include <vector>
#include <array>
#include <set>
#include <string>

class ModuleDAQ : public msm::Module {
    // MSG-driven acquisition: Configure(read config) → Start(init SD+)，ReadOnce/IDLE 驱动 doReadOnce
public:
    static constexpr int MAX_CH = 16;

    ModuleDAQ();
    ~ModuleDAQ();

    void stop() override;
    void handleState(const msm::Message& msg) override;
    void onIdle() override;
    msm::ModuleVars* getVars() override;

    void setRings(const std::vector<UserCircularBuffer*>& rings) { rings_ = rings; }
    UserCircularBuffer* getRing(int idx);
    int getRingCount() const { return (int)rings_.size(); }
    bool isConfigured() const { return configured_.load(); }
    bool isHwReady() const { return hwReady_.load(); }

    struct Stats {
        std::vector<int64_t> total_frames;
        std::vector<size_t> overflow_count;
        std::vector<size_t> frame_size;
        std::vector<int> channel_ids;
        bool is_running = false;
    };
    Stats getStats() const;

private:
    struct ChSlot {
        void* cardHandle = nullptr;
        int ch = 1;
        UserCircularBuffer* ring = nullptr;
        std::atomic<int64_t> totalFrames{0};
        std::atomic<size_t> frameSize{0};
    };

    void onReadConfig(const msm::Json& data);
    bool updateConfig();
    bool onStart();
    void onStop();
    void onReset();
    void onStatus(msm::Message& response);

    bool initHW(std::string& error);
    bool closeHW();
    void doReadOnce();
    void stopChannels();
    void flushAll();

    // 多卡：配置期望卡槽 vs 机箱实际扫描
    std::set<int> expected1073Slots() const;
    int expected3010Slot() const;      // -1 = 配置中无 3010
    int expected3010Count() const;
    bool cardSetMatchesOpen() const;

    std::vector<void*> cards1073_;
    void* card3010_ = nullptr;
    bool sdkReady_ = false;
    std::vector<int> scanned1073Slots_;   // 最近一次机箱扫描结果（诊断用）
    int scanned3010Slot_ = -1;
    std::array<ChSlot, MAX_CH> activeChannels_;
    int activeChCount_ = 0;

    bool setTriggerOutput(bool enable);
    std::vector<UserCircularBuffer*> rings_;

    std::atomic<bool> hwReady_{false};
    std::atomic<bool> acquiring_{false};
    std::atomic<bool> emergencyStop_{false};
    std::atomic<bool> configured_{false};

    DaqConfig config_;

    struct DAQVars : msm::ModuleVars {
        void* get(const std::string& name) override;
        bool set(const std::string& name, const std::string& value) override;
        std::map<std::string, std::string> list() override;
        ModuleDAQ* parent = nullptr;
    } vars_;
};

#endif
