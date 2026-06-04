#ifndef MSM_DAQ_DAQ_MODULE_H
#define MSM_DAQ_DAQ_MODULE_H

#include "Module.h"
#include "user_circular_buffer.h"
#include <atomic>
#include <thread>
#include <cstdint>

class DAQModule : public msm::Module {
public:
    DAQModule();
    ~DAQModule();

    void handleState(const msm::Message& msg) override;
    msm::ModuleVars* getVars() override;

    UserCircularBuffer* getRing(int channelIndex);

    struct Stats {
        int64_t total_frames[4]{};
        size_t overflow_count[4]{};
        size_t frame_size[4]{};
        bool is_running = false;
    };
    Stats getStats() const;

    static constexpr int NCH = 4;
    static constexpr int CHANNELS[4] = {1, 2, 3, 4};

private:
    void onConfigure(const msm::Json& data);
    void onStart();
    void onStop();
    void onReset();
    void onStatus(msm::Message& response);

    void readerLoop();
    void cleanup();

    void* card_ = nullptr;
    std::thread readerThread_;
    std::atomic<bool> readerRunning_{false};

    UserCircularBuffer* rings_[NCH];

    int trigLength_ = 1024;
    std::string voltageRange_ = "1000";
    int trigLevel_ = 200;
    std::string trigSource_ = "4";

    std::atomic<int64_t> totalFrames_[NCH]{};
    std::atomic<size_t> frameSize_[NCH]{};

    struct DAQVars : msm::ModuleVars {
        void* get(const std::string& name) override;
        bool set(const std::string& name, const std::string& value) override;
        std::map<std::string, std::string> list() override;
        DAQModule* parent = nullptr;
    } vars_;
};

#endif
