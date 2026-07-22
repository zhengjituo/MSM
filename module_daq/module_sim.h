#ifndef MSM_DAQ_MODULE_SIM_H
#define MSM_DAQ_MODULE_SIM_H

#include "Module.h"
#include "user_circular_buffer.h"
#include <atomic>
#include <cstdint>
#include <vector>
#include <array>

class ModuleSim : public msm::Module {
public:
    static constexpr int MAX_CH = 16;

#pragma pack(push, 1)
    struct SimMem {
        int trig_length = 0;
        int trigger_rate_hz = 0;
        int frames_per_cycle = 0;
        int running = 0;
        int64_t ch1_frames = 0;
        int64_t ch2_frames = 0;
        int64_t ch3_frames = 0;
        int64_t ch4_frames = 0;
    };
#pragma pack(pop)

    ModuleSim();
    ~ModuleSim();

    void handleState(const msm::Message& msg) override;
    msm::ModuleVars* getVars() override;
    void* getVarBase() override { return &mem_; }
    size_t getVarSize() override { return sizeof(mem_); }

    UserCircularBuffer* getRing(int idx);
    int getRingCount() const { return (int)rings_.size(); }

    struct Stats {
        std::vector<int64_t> total_frames;
        std::vector<size_t> overflow_count;
        std::vector<size_t> frame_size;
        std::vector<int> channel_ids;
        bool is_running = false;
    };
    Stats getStats() const;

private:
    void onConfigure(const msm::Json& data);
    void onStart();
    void onStop();
    void onReset();
    void onStatus(msm::Message& response);

    void doGenerateOnce();
    void buildFrame(uint8_t* buf, size_t frameSize, int ch,
                    uint64_t trigId, uint64_t bunchId);

    std::vector<UserCircularBuffer*> rings_;
    int nCh_ = 0;
    size_t trigLength_ = 1024;
    size_t frameSize_ = 0;
    size_t framesPerCycle_ = 2000;
    size_t triggerRateHz_ = 100000;
    size_t bunchInc_ = 10;

    std::array<std::atomic<int64_t>, MAX_CH> totalFrames_{};
    uint64_t nextTrigId_ = 1;
    uint64_t nextBunchId_ = 1;

    std::atomic<bool> generating_{false};
    std::atomic<bool> emergencyStop_{false};

    std::vector<uint8_t> frameBuf_;
    SimMem mem_;

    struct SimVars : msm::ModuleVars {
        void* get(const std::string& name) override;
        bool set(const std::string& name, const std::string& value) override;
        std::map<std::string, std::string> list() override;
        ModuleSim* parent = nullptr;
    } vars_;
};

#endif
