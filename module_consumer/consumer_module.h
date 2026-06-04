#ifndef MSM_DAQ_CONSUMER_MODULE_H
#define MSM_DAQ_CONSUMER_MODULE_H

#include "Module.h"
#include "user_circular_buffer.h"
#include <atomic>
#include <thread>
#include <cstdint>
#include <vector>

class ConsumerModule : public msm::Module {
public:
    ConsumerModule(const std::string& name, UserCircularBuffer* ring);
    ~ConsumerModule();

    void handleState(const msm::Message& msg) override;
    msm::ModuleVars* getVars() override;

    virtual void onBatch(const uint8_t* data, size_t frameCount, size_t frameSize);

    int64_t getProcessedFrames() const;
    int64_t getProcessedBatches() const;

private:
    void onConfigure(const msm::Json& data);
    void onStart();
    void onStop();
    void onStatus(msm::Message& response);

    void consumerLoop();

    UserCircularBuffer* ring_;
    std::thread consumerThread_;
    std::atomic<bool> running_{false};
    std::atomic<int64_t> processedFrames_{0};
    std::atomic<int64_t> processedBatches_{0};

    size_t batchSize_ = 10000;

    struct ConsumerVars : msm::ModuleVars {
        void* get(const std::string& name) override;
        bool set(const std::string& name, const std::string& value) override;
        std::map<std::string, std::string> list() override;
        ConsumerModule* parent = nullptr;
    } vars_;
};

#endif
