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

    ModuleSim();
    ~ModuleSim();

    void handleState(const msm::Message& msg) override;
    void onIdle() override;
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
    size_t bunchInc_ = 10;  // 1,000,000 / triggerRateHz_

    std::array<std::atomic<int64_t>, MAX_CH> totalFrames_{};
    uint64_t nextTrigId_ = 1;
    uint64_t nextBunchId_ = 1;

    std::atomic<bool> generating_{false};
    std::atomic<bool> emergencyStop_{false};

    std::vector<uint8_t> frameBuf_;

    struct SimVars : msm::ModuleVars {
        void* get(const std::string& name) override;
        bool set(const std::string& name, const std::string& value) override;
        std::map<std::string, std::string> list() override;
        ModuleSim* parent = nullptr;
    } vars_;
};

#endif
