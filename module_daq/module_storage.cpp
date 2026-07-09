#include "module_storage.h"
#include "MessageBus.h"
#include <iostream>
#include <fstream>
#include <sys/stat.h>
#include <cstring>
#include <cerrno>

ModuleStorage::ModuleStorage(const std::vector<UserCircularBuffer*>& rings)
    : Module("Storage"), rings_(rings)
{
    nCh_ = (int)rings_.size();
    if (nCh_ > MAX_CH) {
        std::cerr << "[Storage] WARN: " << nCh_ << " rings > MAX_CH="
                  << MAX_CH << ", truncating\n";
        nCh_ = MAX_CH;
    }
    for (int i = 0; i < nCh_; i++) {
        if (!rings_[i])
            std::cerr << "[Storage] WARN: null ring[" << i << "]\n";
    }
    vars_.parent = this;
    readBuf_.resize(64U * 1024U * 1024U);
    loadState();
}

ModuleStorage::~ModuleStorage() {
    closeFiles();
}

msm::ModuleVars* ModuleStorage::getVars() {
    return &vars_;
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
    if (msg.cmd == "SaveBatchAll") {
        onSaveBatchAll(msg);
    } else if (msg.cmd == "Configure") {
        onConfigure(msg.data);
    } else if (msg.cmd == "Start") {
        onStart();
    } else if (msg.cmd == "Stop") {
        onStop();
    } else if (msg.cmd == "SaveEnable") {
        saveEnabled_.store(true);
        mem_.save_enabled = 1;
        {
            std::lock_guard<std::mutex> lock(fileMtx_);
            openFiles();
        }
        pushPVUpdate();
        std::cout << "[Storage] Save ENABLED\n";
    } else if (msg.cmd == "SaveDisable") {
        saveEnabled_.store(false);
        mem_.save_enabled = 0;
        {
            std::lock_guard<std::mutex> lock(fileMtx_);
            closeFiles();
        }
        pushPVUpdate();
        std::cout << "[Storage] Save DISABLED\n";
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
    if (data.contains("SaveEnabled"))
        saveEnabled_.store(data.get("SaveEnabled") == "true");
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

void ModuleStorage::onStart() {
    std::lock_guard<std::mutex> lock(fileMtx_);
    if (recording_.load()) return;
    loadState();
    sessionId_++;
    for (int i = 0; i < MAX_CH; i++) {
        files_[i].framesSaved.store(0);
        files_[i].bytesSaved.store(0);
    }
    if (saveEnabled_.load())
        openFiles();
    recording_ = true;
    saveState();
    std::cout << "[Storage] Started (session " << sessionId_ << ")"
              << (saveEnabled_.load() ? " saving" : " consume-only") << "\n";

    pushPVUpdate();
}

void ModuleStorage::onStop() {
    {
        std::lock_guard<std::mutex> lock(fileMtx_);
        recording_ = false;
        closeFiles();
    }
    std::cout << "[Storage] Stopped";
    for (int i = 0; i < nCh_; i++)
        std::cout << " ch" << (i + 1) << "=" << files_[i].framesSaved.load();
    std::cout << "\n";
    mem_.recording = 0;
    if (nCh_ > 0) { mem_.ch1_frames = files_[0].framesSaved.load(); mem_.ch1_bytes = files_[0].bytesSaved.load(); }
    if (nCh_ > 1) { mem_.ch2_frames = files_[1].framesSaved.load(); mem_.ch2_bytes = files_[1].bytesSaved.load(); }
    if (nCh_ > 2) { mem_.ch3_frames = files_[2].framesSaved.load(); mem_.ch3_bytes = files_[2].bytesSaved.load(); }
    if (nCh_ > 3) { mem_.ch4_frames = files_[3].framesSaved.load(); mem_.ch4_bytes = files_[3].bytesSaved.load(); }
    pushPVUpdate();
}

void ModuleStorage::onSaveBatchAll(const msm::Message& msg) {
    (void)msg;
    // Always consume from rings to advance read_total_ so Process can continue
    for (int i = 0; i < nCh_; i++) {
        if (!rings_[i]) continue;

        size_t flen = rings_[i]->getExpectedPacketSize();
        size_t needed = batchSize_ * flen;

        if (readBuf_.size() < needed)
            readBuf_.resize(needed);

        size_t got = rings_[i]->readData(readBuf_.data(), needed);
        if (got != needed) {
            std::cerr << "[Storage] ch" << (i + 1) << " read mismatch\n";
            continue;
        }

        // Only write to file if save is enabled
        if (saveEnabled_.load())
            writeToFile(i, readBuf_.data(), batchSize_, flen);
    }

    mem_.recording = recording_.load() ? 1 : 0;
    mem_.save_enabled = saveEnabled_.load() ? 1 : 0;
    if (nCh_ > 0) { mem_.ch1_frames = files_[0].framesSaved.load(); mem_.ch1_bytes = files_[0].bytesSaved.load(); }
    if (nCh_ > 1) { mem_.ch2_frames = files_[1].framesSaved.load(); mem_.ch2_bytes = files_[1].bytesSaved.load(); }
    if (nCh_ > 2) { mem_.ch3_frames = files_[2].framesSaved.load(); mem_.ch3_bytes = files_[2].bytesSaved.load(); }
    if (nCh_ > 3) { mem_.ch4_frames = files_[3].framesSaved.load(); mem_.ch4_bytes = files_[3].bytesSaved.load(); }
    pushPVUpdate();
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
    if (!f.is_open()) {
        sessionId_ = 0;
        return;
    }
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

void ModuleStorage::pushPVUpdate() {
    msm::Message pvMsg("PV_UPDATE");
    pvMsg.data.set("module", name_);
    pvMsg.data.set("var", "*");
    pvMsg.setTarget("PVServer");
    msm::MessageBus::instance().send(pvMsg);
}

void ModuleStorage::onStatus(msm::Message& response) {
    response.data.set("recording", recording_.load() ? "true" : "false");
    response.data.set("save_enabled", saveEnabled_.load() ? "true" : "false");
    for (int i = 0; i < nCh_; i++) {
        response.data.set("ch" + std::to_string(i + 1) + "_frames",
                           std::to_string(files_[i].framesSaved.load()));
    }
}

void* ModuleStorage::StorageVars::get(const std::string& name) {
    return nullptr;
}

bool ModuleStorage::StorageVars::set(const std::string& name, const std::string& value) {
    return false;
}

std::map<std::string, std::string> ModuleStorage::StorageVars::list() {
    return {};
}
