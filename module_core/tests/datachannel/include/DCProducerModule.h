#ifndef MSM_DCPRODUCERMODULE_H
#define MSM_DCPRODUCERMODULE_H

#include "Module.h"
#include "MessageBus.h"
#include "DataChannel.h"

namespace msm {

class DCProducerModule : public Module {
public:
    DCProducerModule(const std::string& name, const std::string& peerName,
                     const std::string& targetModule,
                     int framesPerBatch, int frameSize, long totalBatches,
                     DataChannel* dc);

    void handleState(const Message& msg) override;
    ModuleVars* getVars() override;
    void* getVarBase() override { return &data_; }
    size_t getVarSize() override { return sizeof(data_); }

    long batchesSent() const { return batchesSent_; }
    long bytesSent() const { return bytesSent_; }
    bool isDone() const { return done_; }

private:
    struct {
        int action = 0;
        int frames_per_batch = 1000;
        int frame_size = 128;
        long total_batches = 2000;
        long batches_sent = 0;
        long bytes_sent = 0;
        double throughput_mbs = 0;
        int done = 0;
    } data_;

    std::string peerName_;
    std::string targetModule_;
    DataChannel* dc_;
    long batchesSent_ = 0;
    long bytesSent_ = 0;
    bool started_ = false;
    bool done_ = false;
    std::chrono::steady_clock::time_point tStart_;
};

} // namespace msm

#endif
