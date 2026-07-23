#ifndef MSM_DAQ_MODULE_STORAGE_H
#define MSM_DAQ_MODULE_STORAGE_H

#include "Module.h"
#include "user_circular_buffer.h"
#include <atomic>
#include <string>
#include <array>
#include <vector>
#include <cstdio>
#include <mutex>

class ModuleStorage : public msm::Module {
public:
    static constexpr int MAX_CH = 16;

    ModuleStorage(UserCircularBuffer* saveRing,
                  const std::vector<int>& physChannels);
    ~ModuleStorage();

    void handleState(const msm::Message& msg) override;
    void onIdle() override;

    int64_t getConsumedFrames(int ch) const;
    int64_t getSavedBytes(int ch) const;
    bool isSaveEnabled() const { return saveEnabled_.load(); }
    bool isDraining() const { return draining_.load(); }

private:
    void onConfigure(const msm::Json& data);
    void onStart();
    void onStop();
    void onEnableSave();
    void onDisableSave();
    void onStatus(msm::Message& response);
    void registerVars();

    void closeFiles();
    void rotateFile(int ch);

    std::string buildDirPath() const;
    std::string buildTempPath(int ch, int seq) const;
    std::string buildFinalPath(int ch, int seq, uint64_t firstB,
                               uint64_t lastB) const;
    void ensureDir(const std::string& path) const;
    uint64_t parseBunchID(const uint8_t* frame, size_t flen) const;

    UserCircularBuffer* saveRing_ = nullptr;
    std::vector<int> physChannels_;
    int nCh_ = 0;
    int slot_ = 0;

    // Config
    std::string savePath_ = "/mnt/localssd";
    std::string felName_ = "fel1";
    std::string systemName_ = "HSS";
    std::string deviceName_ = "PD";
    std::string fileType_ = "dat";
    size_t maxSaveSizeMB_ = 1024;
    int expNo_ = 1;
    std::string runSerial_ = "r00001";
    size_t batchSize_ = 10000;
    size_t frameSize_ = 1152;

    // Per-channel state
    struct ChFile {
        FILE* fp = nullptr;
        size_t bytesWritten = 0;
        int sequence = 0;
        std::atomic<int64_t> framesConsumed{0};
        std::atomic<int64_t> bytesSaved{0};
        uint64_t firstBunchID = 0;
        uint64_t lastBunchID = 0;
        bool hasFirstBunch = false;
    };
    ChFile files_[MAX_CH];

    std::string statePath_ = ".";
    int sessionId_ = 0;
    std::atomic<bool> recording_{false};
    std::atomic<bool> draining_{false};
    bool drainHadData_ = false;
    std::atomic<bool> saveEnabled_{false};
    std::mutex fileMtx_;
    std::vector<uint8_t> readBuf_;
};

#endif
