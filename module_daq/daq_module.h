#ifndef MSM_DAQ_DAQ_MODULE_H
#define MSM_DAQ_DAQ_MODULE_H

#include "Module.h"
#include "user_circular_buffer.h"
#include "daq_config.h"
#include <atomic>
#include <thread>
#include <cstdint>
#include <vector>
#include <array>

class DAQModule : public msm::Module {
public:
    static constexpr int MAX_CH = 16;

    DAQModule();
    ~DAQModule();

    void stop() override;
    void handleState(const msm::Message& msg) override;
    msm::ModuleVars* getVars() override;

    UserCircularBuffer* getRing(int idx);
    int getRingCount() const { return (int)rings_.size(); }

    struct Stats {
        std::vector<int64_t> total_frames;
        std::vector<size_t> overflow_count;
        std::vector<size_t> frame_size;
        bool is_running = false;
    };
    Stats getStats() const;

    void loadConfig(const std::string& path);

private:
    struct ChSlot {
        void* cardHandle = nullptr;
        int ch = 1;
        UserCircularBuffer* ring = nullptr;
        std::atomic<int64_t> totalFrames{0};
        std::atomic<size_t> frameSize{0};
    };

    void onConfigure(const msm::Json& data);
    bool onStart();
    void onStop();
    void onReset();
    void onStatus(msm::Message& response);

    void readerLoop();
    void cleanup();
    void flushAll();

    std::vector<void*> cards1073_;
    void* card3010_ = nullptr;
    std::array<ChSlot, MAX_CH> activeChannels_;
    int activeChCount_ = 0;
    std::vector<UserCircularBuffer*> rings_;

    std::thread readerThread_;
    std::atomic<bool> readerRunning_{false};

    DaqConfig config_;

    bool init3010();
    bool setTriggerOutput(bool enable);

    struct DAQVars : msm::ModuleVars {
        void* get(const std::string& name) override;
        bool set(const std::string& name, const std::string& value) override;
        std::map<std::string, std::string> list() override;
        DAQModule* parent = nullptr;
    } vars_;
};

#endif
