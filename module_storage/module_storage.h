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

    ModuleStorage(const std::vector<UserCircularBuffer*>& rings);
    ~ModuleStorage();

    void handleState(const msm::Message& msg) override;
    msm::ModuleVars* getVars() override;

    int64_t getSavedFrames(int ch) const;
    int64_t getSavedBytes(int ch) const;

private:
    void onConfigure(const msm::Json& data);
    void onStart();
    void onStop();
    void onSaveBatch(const msm::Message& msg);
    void onSaveBatchAll(const msm::Message& msg);
    void onStatus(msm::Message& response);

    void writeToFile(int ch, const uint8_t* data, size_t frameCount, size_t frameSize);
    void openFiles();
    void closeFiles();
    void rotateFile(int ch);
    void loadState();
    void saveState();
    std::string formatFilePath(int ch, int seq) const;

    // ring buffer
    std::vector<UserCircularBuffer*> rings_;
    int nCh_ = 0;
    size_t batchSize_ = 10000;

    // file I/O
    std::string savePath_ = "/mnt/localssd";
    std::string fileName_ = "PD";
    std::string fileType_ = "dat";
    size_t maxSaveSizeMB_ = 1024;
    int runId_ = 1;
    std::string filePathTemplate_ = "{savePath}/{fileName}_s{runId}_ch{ch}_{seq}.{fileType}";

    struct ChFile {
        FILE* fp = nullptr;
        size_t bytesWritten = 0;
        int sequence = 0;
        std::atomic<int64_t> framesSaved{0};
        std::atomic<int64_t> bytesSaved{0};
    };
    ChFile files_[MAX_CH];

    std::string statePath_ = ".";
    int sessionId_ = 0;
    std::atomic<bool> recording_{false};
    std::mutex fileMtx_;
    std::vector<uint8_t> readBuf_;

    struct StorageVars : msm::ModuleVars {
        void* get(const std::string& name) override;
        bool set(const std::string& name, const std::string& value) override;
        std::map<std::string, std::string> list() override;
        ModuleStorage* parent = nullptr;
    } vars_;
};

#endif
