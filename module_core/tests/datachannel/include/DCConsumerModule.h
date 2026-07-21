#ifndef MSM_DCCONSUMMERMODULE_H
#define MSM_DCCONSUMMERMODULE_H

#include "Module.h"
#include "MessageBus.h"
#include "DataChannel.h"
#include <vector>

namespace msm {

class DCConsumerModule : public Module {
public:
    DCConsumerModule(const std::string& name, DataChannel* dc);

    void handleState(const Message& msg) override;
    ModuleVars* getVars() override;
    void* getVarBase() override { return &data_; }
    size_t getVarSize() override { return sizeof(data_); }

    long batchesReceived() const { return batchesReceived_; }
    long bytesReceived() const { return bytesReceived_; }
    bool isDone() const { return done_; }

private:
    struct {
        long batches_received = 0;
        long bytes_received = 0;
        double throughput_mbs = 0;
        int errors = 0;
        int done = 0;
    } data_;

    DataChannel* dc_;
    long batchesReceived_ = 0;
    long bytesReceived_ = 0;
    int errors_ = 0;
    bool done_ = false;
    std::chrono::steady_clock::time_point tStart_;
    std::vector<char> buf_;
};

} // namespace msm

#endif
