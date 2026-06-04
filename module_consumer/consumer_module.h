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
    static constexpr int NCH = 4;

    ConsumerModule(const std::string& name,
                   UserCircularBuffer* ring0,
                   UserCircularBuffer* ring1,
                   UserCircularBuffer* ring2,
                   UserCircularBuffer* ring3);
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

    void consumerLoop(int ch, UserCircularBuffer* ring);

    UserCircularBuffer* rings_[NCH];
    std::thread consumerThreads_[NCH];
    std::atomic<bool> running_{false};
    std::atomic<int64_t> processedFrames_[NCH]{};
    std::atomic<int64_t> processedBatches_[NCH]{};

    size_t batchSize_ = 10000;

    struct ConsumerVars : msm::ModuleVars {
        void* get(const std::string& name) override;
        bool set(const std::string& name, const std::string& value) override;
        std::map<std::string, std::string> list() override;
        ConsumerModule* parent = nullptr;
    } vars_;
};

#endif
