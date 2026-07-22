#ifndef MSM_DAQ_MODULE_PROCESS_H
#define MSM_DAQ_MODULE_PROCESS_H

#include "Module.h"
#include "user_circular_buffer.h"
#include <atomic>
#include <cstdint>
#include <vector>
#include <array>

class ModuleProcess : public msm::Module {
public:
    static constexpr int MAX_CH = 16;

    ModuleProcess(const std::string& name,
                  const std::vector<UserCircularBuffer*>& rings);
    ~ModuleProcess();

    void handleState(const msm::Message& msg) override;
    void onIdle() override;
    msm::ModuleVars* getVars() override;

    int64_t getProcessedFrames(int ch) const;
    int64_t getProcessedBatches(int ch) const;
    bool isConsuming() const { return consuming_.load(); }
    int64_t getLastTrigId(int ch) const;
    int64_t getLastBunchId(int ch) const;

    void setStorageTarget(const std::string& target);

private:
    void onConfigure(const msm::Json& data);
    void onStart();
    void onStop();
    void onStatus(msm::Message& response);
    void registerVars();

    void doConsumeOnce();

    std::vector<UserCircularBuffer*> rings_;
    int nCh_ = 0;
    std::array<std::atomic<int64_t>, MAX_CH> processedFrames_{};
    std::array<std::atomic<int64_t>, MAX_CH> processedBatches_{};
    std::array<std::atomic<uint64_t>, MAX_CH> lastWritePos_{};
    std::atomic<bool> consuming_{false};
    std::atomic<bool> emergencyStop_{false};

    size_t batchSize_ = 10000;
    std::string storageTarget_ = "Storage";

    std::array<std::atomic<int64_t>, MAX_CH> lastTrigId_{};
    std::array<std::atomic<int64_t>, MAX_CH> lastBunchId_{};

    std::vector<uint8_t> peekBuf_;
};

#endif
