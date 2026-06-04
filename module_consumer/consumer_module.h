#ifndef MSM_DAQ_CONSUMER_MODULE_H
#define MSM_DAQ_CONSUMER_MODULE_H

#include "Module.h"
#include "user_circular_buffer.h"
#include <atomic>
#include <thread>
#include <cstdint>
#include <vector>
#include <array>

class ConsumerModule : public msm::Module {
public:
    static constexpr int MAX_CH = 16;

    ConsumerModule(const std::string& name,
                   const std::vector<UserCircularBuffer*>& rings);
    ~ConsumerModule();

    void handleState(const msm::Message& msg) override;
    msm::ModuleVars* getVars() override;

    virtual void onBatch(int channel, const uint8_t* data,
                         size_t frameCount, size_t frameSize);

    int64_t getProcessedFrames(int ch) const;
    int64_t getProcessedBatches(int ch) const;

private:
    void onConfigure(const msm::Json& data);
    void onStart();
    void onStop();
    void onStatus(msm::Message& response);

    void consumerLoop(int chIdx, UserCircularBuffer* ring);

    std::vector<UserCircularBuffer*> rings_;
    int nCh_ = 0;
    std::array<std::thread, MAX_CH> consumerThreads_;
    std::array<std::atomic<int64_t>, MAX_CH> processedFrames_{};
    std::array<std::atomic<int64_t>, MAX_CH> processedBatches_{};
    std::atomic<bool> running_{false};

    size_t batchSize_ = 10000;

    struct ConsumerVars : msm::ModuleVars {
        void* get(const std::string& name) override;
        bool set(const std::string& name, const std::string& value) override;
        std::map<std::string, std::string> list() override;
        ConsumerModule* parent = nullptr;
    } vars_;
};

#endif
