#ifndef MSM_DAQ_DAQ_MODULE_H
#define MSM_DAQ_DAQ_MODULE_H

#include "Module.h"
#include "user_circular_buffer.h"
#include "daq_config.h"
#include <atomic>
#include <thread>
#include <cstdint>

class DAQModule : public msm::Module {
public:
    DAQModule();
    ~DAQModule();

    void stop() override;
    void handleState(const msm::Message& msg) override;
    msm::ModuleVars* getVars() override;

    UserCircularBuffer* getRing(int channelIndex);
    void loadConfig(const std::string& path);

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
    bool onStart();
    void onStop();
    void onReset();
    void onStatus(msm::Message& response);

    void readerLoop();
    void cleanup();

    void* card_ = nullptr;
    void* card3010_ = nullptr;
    std::thread readerThread_;
    std::atomic<bool> readerRunning_{false};
    bool init3010();
    bool setTriggerOutput(bool enable);

    UserCircularBuffer* rings_[NCH];
    DaqConfig config_;

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
