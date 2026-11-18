#ifndef MSM_DAQ_MODULE_DAQ_H
#define MSM_DAQ_MODULE_DAQ_H

#include "Module.h"
#include "user_circular_buffer.h"
#include "daq_config.h"
#include <atomic>
#include <thread>
#include <chrono>
#include <cstdint>
#include <vector>
#include <array>
#include <set>
#include <string>

class ModuleDAQ : public msm::Module {
    // MSG-driven acquisition: Configure(read config) → Start(init SD+)，读回由每卡专属读线程驱动
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
    bool isInitFailed() const { return initFailed_.load(); }
    std::string initError() const {
        std::lock_guard<std::mutex> lk(initErrMtx_);
        return initError_;
    }
    void setInitFailed(bool failed, const std::string& err) {
        initFailed_.store(failed);
        std::lock_guard<std::mutex> lk(initErrMtx_);
        initError_ = err;
    }

    struct Stats {
        std::vector<int64_t> total_frames;
        std::vector<size_t> overflow_count;
        std::vector<size_t> frame_size;
        std::vector<int> channel_ids;
        std::vector<int> slots;      // 每通道所在机箱卡槽（-1 未知）
        std::vector<uint64_t> sdk_drops;         // 上游(卡/SDK)丢失帧数（bunch 间隙推断）
        std::vector<uint64_t> ring_drop_frames;  // 本端 ring 满丢弃的累计帧数（精确）
        bool is_running = false;
    };
    Stats getStats() const;

private:
    struct ChSlot {
        void* cardHandle = nullptr;
        int ch = 1;
        int slot = -1;               // 机箱卡槽（来自配置，UI 按卡分组用）
        UserCircularBuffer* ring = nullptr;
        std::atomic<int64_t> totalFrames{0};
        std::atomic<size_t> frameSize{0};
        // API 读回计时（排查双卡阻塞）：SDK 调用 / memcpy+ring 写入，累计 µs + 单次最大 µs
        std::atomic<int64_t> apiCalls{0}, apiUs{0}, apiMaxUs{0};
        std::atomic<int64_t> copyUs{0}, copyMaxUs{0};
        // SDK 返回帧连续性（文档：连续 n 帧）；首次读取验证后决定单次大 memcpy 还是逐帧
        bool contigOk = true;
        bool contigChecked = false;
        // OVERFLOW 告警节流：每通道最多 1 条/秒（steady_clock ms）
        int64_t lastOvfLogMs = 0;
        // 上游（卡/SDK 侧）丢帧：SDK 无计数器，由 bunch_id 序列间隙推断
        std::atomic<uint64_t> upstreamDrops{0};   // 累计缺失帧数
        std::atomic<uint64_t> ringDropFrames{0};  // 本端 ring 满丢弃累计帧数（= 丢弃字节/帧长，精确）
        uint64_t lastBunch = 0;   // 最近读到帧的 bunch_id（跨 pass 连续性）
        uint64_t stepMin = 0;     // 帧间最小 bunch 步长（名义步长；0=未学习）
        int64_t lastUpLogMs = 0;  // UPSTREAM DROP 告警节流（1 条/秒）
    };
    // 每卡一个读线程（多卡单线程串行读 → pass 周期超过 SDK 16MB 缓冲保持时间 → 丢帧）
    std::vector<std::thread> readThreads_;

    void onReadConfig(const msm::Json& data);
    bool updateConfig();
    bool onStart();
    void onStop();
    void onReset();
    void onStatus(msm::Message& response);

    bool initHW(std::string& error);
    bool closeHW();
    void readLoop(int cardNo, const std::vector<int>& chIds);
    void startReadThreads();
    void joinReadThreads();
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
    std::atomic<bool> initFailed_{false};
    mutable std::mutex initErrMtx_;
    std::string initError_;
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
