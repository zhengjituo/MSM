#ifndef MSM_DAQ_MODULE_PROCESS_H
#define MSM_DAQ_MODULE_PROCESS_H

#include "Module.h"
#include "user_circular_buffer.h"
#include "frame_parser.h"
#include <atomic>
#include <mutex>
#include <cstdint>
#include <vector>
#include <array>
#include <string>

class ModuleProcess : public msm::Module {
public:
    static constexpr int MAX_CH = 16;

    struct BatchFrameData {
        std::vector<uint32_t> trig_id;
        std::vector<uint64_t> bunch_id;
        std::vector<double> min_val;
        std::vector<double> max_val;
        std::vector<double> sum_l;
        std::vector<double> sum_h;
        std::vector<double> vpp;
        std::vector<double> x;
        std::vector<double> y;
        std::vector<double> i;
        std::vector<uint16_t> wave_raw;
        uint16_t wave_offset = 0;
        uint32_t wave_scale = 0;
        uint16_t wave_range = 0;
        int64_t batchNo = 0;
        size_t count = 0;
    };

    ModuleProcess(const std::string& name,
                      const std::vector<UserCircularBuffer*>& rings,
                      UserCircularBuffer* saveRing = nullptr,
                      const std::vector<int>& physChannels = {});
    ~ModuleProcess();

    void handleState(const msm::Message& msg) override;
    void onIdle() override;
    msm::ModuleVars* getVars() override;

    int64_t getProcessedFrames(int ch) const;
    int64_t getProcessedBatches(int ch) const;
    bool isConsuming() const { return consuming_.load(); }
    int64_t getLastTrigId(int ch) const;
    int64_t getLastBunchId(int ch) const;
    double  getLastSumH(int ch) const;
    double  getLastX() const;
    double  getLastY() const;
    double  getLastI() const;

    BatchFrameData getLastBatchData(int ch) const;
    mutable std::mutex batchMutex_;

    void setStorageTarget(const std::string& target);
    void setFrameIndex(int idx);
    int  getFrameIndex() const { return frameIndex_.load(); }

private:
    void onConfigure(const msm::Json& data);
    void onStart();
    void onStop();
    void onStopImmediate();
    void onStatus(msm::Message& response);
    void registerVars();

    void doConsumeOnce();
    void extractFrame(const uint8_t* frame, size_t flen, int ch);

    int detectBunchIdStep(const uint8_t* batchBuf, size_t flen, size_t nFrames) const;

    std::vector<UserCircularBuffer*> rings_;
    UserCircularBuffer* saveRing_ = nullptr;
    std::vector<int> physChannels_;
    int nCh_ = 0;
    std::array<std::atomic<int64_t>, MAX_CH> processedFrames_{};
    std::array<std::atomic<int64_t>, MAX_CH> processedBatches_{};
    std::array<std::atomic<uint64_t>, MAX_CH> lastWritePos_{};
    std::array<int64_t, MAX_CH> lastPeekedReadPos_{};

    std::atomic<bool> consuming_{false};
    std::atomic<bool> emergencyStop_{false};
    std::atomic<bool> flushing_{false};
    // 保存开关：saveFlag_ 为待生效（Start 时应用，与 Storage「下次 start 生效」语义一致）；
    // 关闭时 Process 不写 saveRing，避免「写 ring → Storage 丢弃」的纯浪费拷贝
    std::atomic<bool> saveFlag_{false};
    std::atomic<bool> saveEnabled_{false};

    size_t batchSize_ = 10000;
    size_t frameSize_ = 0;
    std::string storageTarget_ = "Storage";

    std::array<std::atomic<int64_t>, MAX_CH> lastTrigId_{};
    std::array<std::atomic<int64_t>, MAX_CH> lastBunchId_{};
    std::array<std::atomic<double>, MAX_CH> lastSumH_{};
    std::array<std::atomic<double>, MAX_CH> lastX_{};
    std::array<std::atomic<double>, MAX_CH> lastY_{};
    std::array<std::atomic<double>, MAX_CH> lastI_{};

    std::array<BatchFrameData, MAX_CH> batchData_;
    std::vector<uint8_t> batchBuf_;
    std::atomic<int> frameIndex_{-1};  // -1 = auto-align to 100ms boundary
    int bunchIdStep_ = 10;
};

#endif
