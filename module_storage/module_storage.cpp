#include "module_storage.h"
#include "MessageBus.h"
#include <iostream>
#include <fstream>
#include <sys/stat.h>
#include <cstring>
#include <cerrno>

ModuleStorage::ModuleStorage(UserCircularBuffer* saveRing,
                             const std::vector<int>& physChannels)
    : Module("Storage"), saveRing_(saveRing), physChannels_(physChannels)
{
    nCh_ = (int)physChannels.size();
    if (nCh_ > MAX_CH) {
        std::cerr << "[Storage] WARN: " << nCh_ << " > MAX_CH="
                  << MAX_CH << ", truncating\n";
        nCh_ = MAX_CH;
    }
    idleDelayMs_ = 50;
    readBuf_.resize(64U * 1024U * 1024U);
    registerVars();
    loadState();
}

ModuleStorage::~ModuleStorage() {
    closeFiles();
}

int64_t ModuleStorage::getSavedFrames(int ch) const {
    if (ch < 0 || ch >= MAX_CH) return 0;
    return files_[ch].framesSaved.load();
}

int64_t ModuleStorage::getSavedBytes(int ch) const {
    if (ch < 0 || ch >= MAX_CH) return 0;
    return files_[ch].bytesSaved.load();
}

void ModuleStorage::handleState(const msm::Message& msg) {
    if (msg.cmd == "Configure") {
        onConfigure(msg.data);
    } else if (msg.cmd == "Start") {
        onStart();
    } else if (msg.cmd == "Stop") {
        onStop();
    } else if (msg.cmd == "Status") {
        msm::Message response("Response");
        response.setTarget(msg.getSender());
        response.setCorrelationId(msg.getCorrelationId());
        onStatus(response);
        msm::MessageBus::instance().send(response);
    }
}

void ModuleStorage::onConfigure(const msm::Json& data) {
    if (data.contains("SavePath"))
        savePath_ = data.get("SavePath");
    if (data.contains("FileName"))
        fileName_ = data.get("FileName");
    if (data.contains("FileType"))
        fileType_ = data.get("FileType");
    if (data.contains("MaxSaveSizeMB"))
        maxSaveSizeMB_ = static_cast<size_t>(data.getInt("MaxSaveSizeMB"));
    if (data.contains("RunId"))
        runId_ = data.getInt("RunId");
    if (data.contains("StateDir"))
        statePath_ = data.get("StateDir");
    if (data.contains("BatchSize"))
        batchSize_ = static_cast<size_t>(data.getInt("BatchSize"));
    if (data.contains("FilePathTemplate"))
        filePathTemplate_ = data.get("FilePathTemplate");
    size_t maxFl = 1152;
    size_t need = batchSize_ * maxFl;
    if (readBuf_.size() < need)
        readBuf_.resize(need);
    std::cout << "[Storage] Configured: path=" << savePath_
              << " prefix=" << fileName_
              << " maxMB=" << maxSaveSizeMB_
              << " batch=" << batchSize_
              << " nCh=" << nCh_ << "\n";
}

void ModuleStorage::onIdle() {
    if (!recording_.load() && !draining_.load()) return;
    if (!saveRing_) return;

    size_t batchBytes = batchSize_ * 384;  // per-channel batch size
    size_t totalNeeded = (size_t)nCh_ * batchBytes;
    size_t avail = saveRing_->availableDataSize();

    if (avail < totalNeeded) {
        // not enough data for a full multi-channel batch
        if (draining_.load() && avail == 0) {
            draining_ = false;
            recording_ = false;
            std::lock_guard<std::mutex> lock(fileMtx_);
            closeFiles();
            std::cout << "[Storage] Drain complete\n";
        }
        return;
    }

    if (readBuf_.size() < totalNeeded)
        readBuf_.resize(totalNeeded);

    size_t got = saveRing_->readData(readBuf_.data(), totalNeeded);
    if (got < totalNeeded) return;

    // split by channel, write each to its own file
    for (int i = 0; i < nCh_; i++) {
        const uint8_t* chData = readBuf_.data() + i * batchBytes;
        int chPhys = (i < (int)physChannels_.size()) ? physChannels_[i] : (i + 1);
        if (recording_.load() && files_[i].fp) {
            fwrite(chData, 1, batchBytes, files_[i].fp);
            files_[i].bytesWritten += batchBytes;
            files_[i].framesSaved.fetch_add((int64_t)batchSize_);
            files_[i].bytesSaved.fetch_add((int64_t)batchBytes);
            if (maxSaveSizeMB_ > 0 &&
                files_[i].bytesWritten >= maxSaveSizeMB_ * 1024ULL * 1024ULL)
                rotateFile(i);
        }
    }
}

void ModuleStorage::onStart() {
    std::lock_guard<std::mutex> lock(fileMtx_);
    if (recording_.load()) return;
    for (int i = 0; i < nCh_; i++) {
        files_[i].framesSaved.store(0);
        files_[i].bytesSaved.store(0);
    }
    loadState();
    sessionId_++;
    openFiles();
    recording_ = true;
    saveState();
    std::cout << "[Storage] Started (session " << sessionId_ << ")\n";
}

void ModuleStorage::onStop() {
    // drain mode: process remaining saveRing data
    draining_ = true;
    std::cout << "[Storage] Draining remaining data...\n";
}



void ModuleStorage::onStatus(msm::Message& response) {
    response.data.set("recording", recording_.load() ? "true" : "false");
    response.data.set("session", std::to_string(sessionId_));
    for (int i = 0; i < nCh_; i++) {
        response.data.set("ch" + std::to_string(i + 1) + "_saved_frames",
                          std::to_string(files_[i].framesSaved.load()));
        response.data.set("ch" + std::to_string(i + 1) + "_saved_bytes",
                          std::to_string(files_[i].bytesSaved.load()));
    }
}



void ModuleStorage::writeToFile(int ch, const uint8_t* data,
                                 size_t frameCount, size_t frameSize)
{
    if (ch < 0 || ch >= MAX_CH) return;
    size_t bytes = frameCount * frameSize;
    std::lock_guard<std::mutex> lock(fileMtx_);
    if (!recording_.load()) return;
    auto& f = files_[ch];
    if (!f.fp) return;
    size_t written = fwrite(data, 1, bytes, f.fp);
    if (written != bytes) {
        std::cerr << "[Storage] ch" << (ch + 1)
                  << " write error: " << strerror(errno) << "\n";
        return;
    }
    f.bytesWritten += written;
    f.framesSaved.fetch_add(frameCount);
    f.bytesSaved.fetch_add(written);
    if (f.bytesWritten >= maxSaveSizeMB_ * 1024U * 1024U)
        rotateFile(ch);
}

void ModuleStorage::openFiles() {
    mkdir(savePath_.c_str(), 0755);
    for (int i = 0; i < nCh_ && i < MAX_CH; i++) {
        auto& f = files_[i];
        f.sequence = 0;
        f.bytesWritten = 0;
        std::string path = formatFilePath(i, f.sequence);
        f.fp = fopen(path.c_str(), "wb");
        if (!f.fp) {
            std::cerr << "[Storage] Failed to open " << path
                      << ": " << strerror(errno) << "\n";
        } else {
            std::cout << "[Storage] " << path << "\n";
        }
    }
}

void ModuleStorage::closeFiles() {
    for (int i = 0; i < MAX_CH; i++) {
        auto& f = files_[i];
        if (f.fp) {
            fclose(f.fp);
            f.fp = nullptr;
        }
        f.bytesWritten = 0;
    }
}

void ModuleStorage::rotateFile(int ch) {
    if (ch < 0 || ch >= MAX_CH) return;
    auto& f = files_[ch];
    if (!f.fp) return;
    fclose(f.fp);
    f.fp = nullptr;
    f.sequence++;
    f.bytesWritten = 0;
    std::string path = formatFilePath(ch, f.sequence);
    f.fp = fopen(path.c_str(), "wb");
    if (!f.fp) {
        std::cerr << "[Storage] Rotate failed: " << path
                  << ": " << strerror(errno) << "\n";
    } else {
        std::cout << "[Storage] Rotate -> " << path << "\n";
    }
}

void ModuleStorage::loadState() {
    std::string path = statePath_ + "/.msm_session";
    std::ifstream f(path);
    if (!f.is_open()) { sessionId_ = 0; return; }
    std::string line;
    while (std::getline(f, line)) {
        if (line.compare(0, 11, "session_id=") == 0)
            sessionId_ = std::stoi(line.substr(11));
    }
}

void ModuleStorage::saveState() {
    std::string path = statePath_ + "/.msm_session";
    std::ofstream f(path);
    if (!f.is_open()) {
        std::cerr << "[Storage] Cannot write " << path << "\n";
        return;
    }
    f << "session_id=" << sessionId_ << "\n";
}

std::string ModuleStorage::formatFilePath(int ch, int seq) const {
    std::string result = filePathTemplate_;
    auto replace = [&](const std::string& key, const std::string& val) {
        size_t pos;
        while ((pos = result.find(key)) != std::string::npos)
            result.replace(pos, key.size(), val);
    };
    replace("{savePath}", savePath_);
    replace("{fileName}", fileName_);
    replace("{fileType}", fileType_);
    replace("{runId}", std::to_string(runId_));
    replace("{session}", std::to_string(sessionId_));
    replace("{ch}", std::to_string(ch + 1));
    replace("{seq}", std::to_string(seq));
    return result;
}

void ModuleStorage::registerVars() {
    declareVar("recording", recording_);
    declareVar("save_path", savePath_);
    declareVar("run_id", runId_);
    int maxCh = nCh_ > 0 ? nCh_ : MAX_CH;
    for (int i = 0; i < maxCh; i++) {
        std::string idx = std::to_string(i + 1);
        declareVar("ch" + idx + "_saved_frames", files_[i].framesSaved);
        declareVar("ch" + idx + "_saved_bytes", files_[i].bytesSaved);
    }
}
