#include "module_storage.h"
#include "MessageBus.h"
#include <iostream>
#include <fstream>
#include <sys/stat.h>
#include <cstring>
#include <cerrno>
#include <ctime>
#include <inttypes.h>
#include <unistd.h>

static inline uint32_t readU32BE(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

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
}

ModuleStorage::~ModuleStorage() {
    closeFiles();
}

int64_t ModuleStorage::getConsumedFrames(int ch) const {
    if (ch < 0 || ch >= MAX_CH) return 0;
    return files_[ch].framesConsumed.load();
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
    } else if (msg.cmd == "EnableSave") {
        onEnableSave();
    } else if (msg.cmd == "DisableSave") {
        onDisableSave();
    } else if (msg.cmd == "Status") {
        msm::Message response("Response");
        response.setTarget(msg.getSender());
        response.setCorrelationId(msg.getCorrelationId());
        onStatus(response);
        msm::MessageBus::instance().send(response);
    }
}

void ModuleStorage::onConfigure(const msm::Json& data) {
    if (data.contains("SavePath"))       savePath_ = data.get("SavePath");
    if (data.contains("FelName"))        felName_ = data.get("FelName");
    if (data.contains("SystemName"))     systemName_ = data.get("SystemName");
    if (data.contains("DeviceName"))     deviceName_ = data.get("DeviceName");
    if (data.contains("FileType"))       fileType_ = data.get("FileType");
    if (data.contains("MaxSaveSizeMB"))  maxSaveSizeMB_ = data.getInt("MaxSaveSizeMB");
    if (data.contains("ExpNo"))          expNo_ = data.getInt("ExpNo");
    if (data.contains("RunSerial"))      runSerial_ = data.get("RunSerial");
    if (data.contains("BatchSize"))      batchSize_ = data.getInt("BatchSize");
    if (data.contains("FrameSize"))      frameSize_ = data.getInt("FrameSize");
    if (data.contains("Slot"))           slot_ = data.getInt("Slot");
    size_t need = (size_t)nCh_ * batchSize_ * frameSize_;
    if (readBuf_.size() < need)
        readBuf_.resize(need);
    std::cout << "[Storage] Configured: " << buildDirPath()
              << " batch=" << batchSize_ << " frame=" << frameSize_
              << " maxMB=" << maxSaveSizeMB_ << " nCh=" << nCh_ << "\n";
}

void ModuleStorage::onIdle() {
    if (!saveRing_) return;
    if (frameSize_ == 0) return;

    size_t batchBytes = batchSize_ * frameSize_;
    size_t totalNeeded = (size_t)nCh_ * batchBytes;
    size_t avail = saveRing_->availableDataSize();

    if (!recording_.load() && !draining_.load()) {
        if (avail > 0) {
            if (readBuf_.size() < avail)
                readBuf_.resize(avail);
            saveRing_->readData(readBuf_.data(), avail);
            int64_t discardFrames = (int64_t)avail / (int64_t)nCh_ / (int64_t)frameSize_;
            for (int i = 0; i < nCh_; i++)
                files_[i].framesConsumed.fetch_add(discardFrames);
        }
        return;
    }

    if (draining_.load()) {
        if (avail > 0) {
            drainHadData_ = true;
            while (avail >= totalNeeded) {
                if (readBuf_.size() < totalNeeded)
                    readBuf_.resize(totalNeeded);
                size_t got = saveRing_->readData(readBuf_.data(), totalNeeded);
                if (got < totalNeeded) { avail = 0; break; }
                avail -= totalNeeded;
                if (!recording_.load()) continue;
                int64_t curBatch = files_[0].framesConsumed.load() / (int64_t)batchSize_ + 1;
                for (int i = 0; i < nCh_; i++) {
                    const uint8_t* chData = readBuf_.data() + i * batchBytes;
                    auto& f = files_[i];
                    if (!f.fp) continue;
                    size_t written = fwrite(chData, 1, batchBytes, f.fp);
                    if (written == batchBytes) {
                        f.bytesWritten += written;
                        f.framesConsumed.fetch_add((int64_t)batchSize_);
                        f.bytesSaved.fetch_add((int64_t)written);
                    }
                }
                if (maxSaveSizeMB_ > 0) {
                    size_t limit = maxSaveSizeMB_ * 1024ULL * 1024ULL;
                    for (int i = 0; i < nCh_; i++)
                        if (files_[i].bytesWritten >= limit) {
                            std::lock_guard<std::mutex> lock(fileMtx_);
                            for (int j = 0; j < nCh_; j++) rotateFile(j);
                            break;
                        }
                }
            }
            if (avail > 0) {
                if (readBuf_.size() < avail)
                    readBuf_.resize(avail);
                saveRing_->readData(readBuf_.data(), avail);
                if (recording_.load()) {
                    size_t perCh = avail / (size_t)nCh_;
                    for (int i = 0; i < nCh_; i++) {
                        auto& f = files_[i];
                        if (!f.fp) continue;
                        size_t written = fwrite(readBuf_.data() + i * perCh, 1, perCh, f.fp);
                        if (written != perCh)
                            std::cerr << "[Storage] ch" << (i + 1)
                                      << " partial write error: " << strerror(errno) << "\n";
                        else
                            f.framesConsumed.fetch_add((int64_t)(perCh / frameSize_));
                    }
                    std::cout << "[Storage] drain saved " << (avail / nCh_ / frameSize_)
                              << " frames/ch\n";
                } else {
                    int64_t discardFrames = avail / (int64_t)nCh_ / (int64_t)frameSize_;
                    for (int i = 0; i < nCh_; i++)
                        files_[i].framesConsumed.fetch_add(discardFrames);
                    std::cout << "[Storage] drain discarded " << discardFrames
                              << " frames/ch (save disabled)\n";
                }
            }
        }
        if (avail == 0) {
            draining_ = false;
            recording_ = false;
            {
                std::lock_guard<std::mutex> lock(fileMtx_);
                closeFiles();
            }
            std::cout << "[Storage] drain:";
            for (int ci = 0; ci < nCh_ && ci < MAX_CH; ci++)
                std::cout << " ch" << (ci + 1) << "=" << files_[ci].framesConsumed.load();
            std::cout << "\n";
            drainHadData_ = false;
        }
        return;
    }

    while (saveRing_->availableDataSize() >= totalNeeded) {
        if (readBuf_.size() < totalNeeded)
            readBuf_.resize(totalNeeded);

        size_t got = saveRing_->readData(readBuf_.data(), totalNeeded);
        if (got < totalNeeded) break;

        int64_t curBatch = files_[0].framesConsumed.load() / (int64_t)batchSize_ + 1;

        for (int i = 0; i < nCh_; i++) {
            const uint8_t* chData = readBuf_.data() + i * batchBytes;
            auto& f = files_[i];
            int chPhys = (i < (int)physChannels_.size()) ? physChannels_[i] : (i + 1);

            uint32_t firstTrig = 0;
            uint64_t firstBunch = 0;
            size_t cnt = 0;
            if (chData[0] == 0xEB && chData[1] == 0x91 && frameSize_ >= 72) {
                firstTrig = readU32BE(chData + 17);
                firstBunch = parseBunchID(chData, frameSize_);
                cnt = batchSize_;
            }
            std::cout << "[Storage] ch" << chPhys
                      << " batch=" << curBatch
                      << " trig_id=" << firstTrig
                      << " bunch_id=" << firstBunch
                      << " cnt=" << cnt
                      << "\n";

            if (!f.fp) continue;

            if (!f.hasFirstBunch) {
                f.firstBunchID = parseBunchID(chData, frameSize_);
                f.hasFirstBunch = true;
            }

            const uint8_t* lastFrame = chData + (batchSize_ - 1) * frameSize_;
            f.lastBunchID = parseBunchID(lastFrame, frameSize_);

            size_t written = fwrite(chData, 1, batchBytes, f.fp);
            if (written != batchBytes) {
                std::cerr << "[Storage] ch" << (i + 1) << " write error: "
                          << strerror(errno) << "\n";
                continue;
            }
            f.bytesWritten += written;
            f.framesConsumed.fetch_add((int64_t)batchSize_);
            f.bytesSaved.fetch_add((int64_t)written);
        }

        bool anyRotate = false;
        size_t limitBytes = maxSaveSizeMB_ * 1024ULL * 1024ULL;
        if (maxSaveSizeMB_ > 0) {
            for (int i = 0; i < nCh_; i++) {
                if (files_[i].bytesWritten >= limitBytes) {
                    anyRotate = true;
                    break;
                }
            }
        }
        if (anyRotate) {
            std::lock_guard<std::mutex> lock(fileMtx_);
            for (int i = 0; i < nCh_; i++)
                rotateFile(i);
        }
    }
}

void ModuleStorage::onStart() {
    for (int i = 0; i < nCh_; i++) {
        files_[i].framesConsumed.store(0);
        files_[i].bytesSaved.store(0);
        files_[i].firstBunchID = 0;
        files_[i].lastBunchID = 0;
        files_[i].hasFirstBunch = false;
        files_[i].sequence = 0;
        files_[i].bytesWritten = 0;
    }
    if (saveEnabled_.load() && nCh_ > 0) {
        std::lock_guard<std::mutex> lock(fileMtx_);
        std::string dir = buildDirPath();
        ensureDir(dir);
        for (int i = 0; i < nCh_ && i < MAX_CH; i++) {
            auto& f = files_[i];
            std::string path = buildTempPath(i, f.sequence);
            f.fp = fopen(path.c_str(), "wb");
            if (!f.fp)
                std::cerr << "[Storage] Failed to open " << path << ": " << strerror(errno) << "\n";
            else
                std::cout << "[Storage] " << path << "\n";
        }
        recording_ = true;
        std::cout << "[Storage] Started, recording\n";
    } else {
        std::cout << "[Storage] Started, save disabled, discarding\n";
    }
}

void ModuleStorage::onEnableSave() {
    saveEnabled_ = true;
    std::cout << "[Storage] Save flag ON (applied at next start)\n";
}

void ModuleStorage::onDisableSave() {
    saveEnabled_ = false;
    std::cout << "[Storage] Save flag OFF (applied at next start)\n";
}

void ModuleStorage::onStop() {
    draining_ = true;
    std::cout << "[Storage] Draining remaining data...\n";
}

void ModuleStorage::onStatus(msm::Message& response) {
    response.data.set("recording", recording_.load() ? "true" : "false");
    response.data.set("session", std::to_string(sessionId_));
    for (int i = 0; i < nCh_; i++) {
        response.data.set("ch" + std::to_string(i + 1) + "_saved_frames",
                          std::to_string(files_[i].framesConsumed.load()));
        response.data.set("ch" + std::to_string(i + 1) + "_saved_bytes",
                          std::to_string(files_[i].bytesSaved.load()));
    }
}

void ModuleStorage::closeFiles() {
    for (int i = 0; i < MAX_CH; i++) {
        auto& f = files_[i];
        if (!f.fp) continue;
        fclose(f.fp);
        f.fp = nullptr;
        int ch = i;
        if (f.framesConsumed.load() > 0) {
            std::string tmpPath = buildTempPath(ch, f.sequence);
            std::string finalPath = buildFinalPath(ch, f.sequence,
                f.hasFirstBunch ? f.firstBunchID : 0,
                f.lastBunchID);
            if (rename(tmpPath.c_str(), finalPath.c_str()) == 0)
                std::cout << "[Storage] -> " << finalPath << "\n";
            else
                std::cerr << "[Storage] rename failed: " << strerror(errno) << "\n";
        } else {
            std::string tmpPath = buildTempPath(ch, f.sequence);
            remove(tmpPath.c_str());
        }
        f.bytesWritten = 0;
        f.firstBunchID = 0;
        f.lastBunchID = 0;
        f.hasFirstBunch = false;
    }
}

void ModuleStorage::rotateFile(int ch) {
    if (ch < 0 || ch >= MAX_CH) return;
    auto& f = files_[ch];
    if (!f.fp) return;
    fclose(f.fp);
    f.fp = nullptr;
    std::string tmpPath = buildTempPath(ch, f.sequence);
    std::string finalPath = buildFinalPath(ch, f.sequence,
        f.hasFirstBunch ? f.firstBunchID : 0,
        f.lastBunchID);
    if (f.framesConsumed.load() > 0) {
        if (rename(tmpPath.c_str(), finalPath.c_str()) == 0)
            std::cout << "[Storage] Rotate -> " << finalPath << "\n";
        else
            std::cerr << "[Storage] rename failed: " << strerror(errno) << "\n";
    } else {
        remove(tmpPath.c_str());
    }
    f.sequence++;
    f.bytesWritten = 0;
    f.firstBunchID = 0;
    f.lastBunchID = 0;
    f.hasFirstBunch = false;
    std::string newPath = buildTempPath(ch, f.sequence);
    f.fp = fopen(newPath.c_str(), "wb");
    if (!f.fp)
        std::cerr << "[Storage] Rotate open failed: " << newPath << ": " << strerror(errno) << "\n";
    else
        std::cout << "[Storage] " << newPath << "\n";
}

std::string ModuleStorage::buildDirPath() const {
    time_t t = time(nullptr);
    struct tm lt; localtime_r(&t, &lt);
    char dateBuf[16];
    strftime(dateBuf, sizeof(dateBuf), "%Y%m%d", &lt);

    std::string path = savePath_ + "/" + felName_
        + "/" + systemName_ + "/" + deviceName_
        + "/" + dateBuf + "/" + runSerial_
        + "/card" + std::to_string(slot_);
    return path;
}

std::string ModuleStorage::buildTempPath(int ch, int seq) const {
    char expBuf[16], seqBuf[16];
    snprintf(expBuf, sizeof(expBuf), "%05d", expNo_);
    snprintf(seqBuf, sizeof(seqBuf), "%05d", seq);
    int physCh = (ch < (int)physChannels_.size()) ? physChannels_[ch] : (ch + 1);
    return buildDirPath() + "/RAW-R" + expBuf
        + "-" + systemName_ + "-" + deviceName_
        + "-Channel" + std::to_string(physCh)
        + "-S" + seqBuf + ".tmp";
}

std::string ModuleStorage::buildFinalPath(int ch, int seq,
                                          uint64_t firstB, uint64_t lastB) const {
    char expBuf[16], seqBuf[16], firstBBuf[16], lastBBuf[16];
    snprintf(expBuf, sizeof(expBuf), "%05d", expNo_);
    snprintf(seqBuf, sizeof(seqBuf), "%05d", seq);
    snprintf(firstBBuf, sizeof(firstBBuf), "%014" PRIu64, firstB);
    snprintf(lastBBuf, sizeof(lastBBuf), "%014" PRIu64, lastB);
    int physCh = (ch < (int)physChannels_.size()) ? physChannels_[ch] : (ch + 1);
    return buildDirPath() + "/RAW-R" + expBuf
        + "-" + systemName_ + "-" + deviceName_
        + "-Channel" + std::to_string(physCh)
        + "-B" + firstBBuf + "-B" + lastBBuf
        + "-S" + seqBuf + "." + fileType_;
}

void ModuleStorage::ensureDir(const std::string& path) const {
    size_t pos = 0;
    while ((pos = path.find_first_of('/', pos + 1)) != std::string::npos) {
        std::string sub = path.substr(0, pos);
        mkdir(sub.c_str(), 0755);
    }
    // Also create the final directory component if path doesn't end with /
    if (!path.empty() && path.back() != '/')
        mkdir(path.c_str(), 0755);
}

uint64_t ModuleStorage::parseBunchID(const uint8_t* frame, size_t flen) const {
    const uint8_t* trailer = frame + flen - 64;
    return ((uint64_t)trailer[2] << 40) | ((uint64_t)trailer[3] << 32)
         | ((uint64_t)trailer[4] << 24) | ((uint64_t)trailer[5] << 16)
         | ((uint64_t)trailer[6] << 8)  | trailer[7];
}

void ModuleStorage::registerVars() {
    declareVar("recording", recording_);
    declareVar("save_path", savePath_);
    int maxCh = nCh_ > 0 ? nCh_ : MAX_CH;
    for (int i = 0; i < maxCh; i++) {
        std::string idx = std::to_string(i + 1);
        declareVar("ch" + idx + "_consumed_frames", files_[i].framesConsumed);
        declareVar("ch" + idx + "_saved_bytes", files_[i].bytesSaved);
    }
}
