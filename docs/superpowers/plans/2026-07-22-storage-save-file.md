# Storage V2 — 保存文件功能 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement binary raw file saving with batch-aligned rotation, BunchID-annotated filenames, and correct module start/stop sequencing.

**Architecture:** Expand `ModuleStorage` with new config fields (FelName, SystemName, DeviceName, ExpNo), BunchID tracking per file, and a `buildFilePath()` that constructs the hierarchical directory path + filename. Create a shared `saveRing` in V1 CLI and pass to both ProcessFull and Storage. Wire Start/Stop messages for Storage before Process/DAQModule.

**Tech Stack:** C++17, fwrite, rename(), UserCircularBuffer (shared ring)

---

### Task 1: Update ModuleStorage header with new fields and methods

**Files:**
- Modify: `module_storage/module_storage.h`

- [ ] **Step 1: Replace old fields and add new ones**

Replace the old file-naming fields (savePath_, fileName_, fileType_, maxSaveSizeMB_, runId_, filePathTemplate_) with the new ones, add BunchID tracking arrays, and declare new helper methods.

```cpp
// ── Config ──
std::string savePath_ = "/mnt/localssd";
std::string felName_ = "fel1";
std::string systemName_ = "HSS";
std::string deviceName_ = "PD";
std::string fileType_ = "dat";
size_t maxSaveSizeMB_ = 1024;
int expNo_ = 1;
std::string runSerial_ = "r00001";
size_t batchSize_ = 10000;
size_t frameSize_ = 1152;           // per-channel frame size, set via Configure

// ── Per-channel state ──
struct ChFile {
    FILE* fp = nullptr;
    size_t bytesWritten = 0;
    int sequence = 0;
    std::atomic<int64_t> framesSaved{0};
    std::atomic<int64_t> bytesSaved{0};
    uint64_t firstBunchID = 0;      // first frame BunchID in current file
    uint64_t lastBunchID = 0;       // last frame BunchID in current file
    bool hasFirstBunch = false;     // true after first batch written
};
ChFile files_[MAX_CH];

// ── State ──
std::string statePath_ = ".";
int sessionId_ = 0;
std::atomic<bool> recording_{false};
std::atomic<bool> draining_{false};
std::mutex fileMtx_;
std::vector<uint8_t> readBuf_;

// ── New helpers (replace formatFilePath) ──
std::string buildDirPath() const;
std::string buildTempPath(int ch, int seq) const;
std::string buildFinalPath(int ch, int seq, uint64_t firstB,
                           uint64_t lastB) const;
void ensureDir(const std::string& path) const;
uint64_t parseBunchID(const uint8_t* frame, size_t flen) const;
```

Remove old member: `std::string filePathTemplate_`

- [ ] **Step 2: Commit**

```bash
git add module_storage/module_storage.h
git commit -m "storage: update header with new fields and BunchID tracking"
```

---

### Task 2: Rewrite file path construction

**Files:**
- Modify: `module_storage/module_storage.cpp` (add new helper implementations, remove old `formatFilePath`)

- [ ] **Step 1: Add helper implementations**

```cpp
std::string ModuleStorage::buildDirPath() const {
    // /mnt/localssd/fel1/HSS/PD/20260722/r00001/card3/
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
}

uint64_t ModuleStorage::parseBunchID(const uint8_t* frame, size_t flen) const {
    // Trailer[+2..+7] = BunchID (48-bit big-endian)
    const uint8_t* trailer = frame + flen - 64;
    return ((uint64_t)trailer[2] << 40) | ((uint64_t)trailer[3] << 32)
         | ((uint64_t)trailer[4] << 24) | ((uint64_t)trailer[5] << 16)
         | ((uint64_t)trailer[6] << 8)  | trailer[7];
}
```

Add `#include <ctime>` and `#include <inttypes.h>` at top of file.

- [ ] **Step 2: Remove old `formatFilePath`**

Delete the entire `ModuleStorage::formatFilePath` method.

- [ ] **Step 3: Commit**

```bash
git add module_storage/module_storage.cpp
git commit -m "storage: new file path construction with hierarchical dirs + BunchID"
```

---

### Task 3: Rewrite onConfigure with new fields

**Files:**
- Modify: `module_storage/module_storage.cpp`

- [ ] **Step 1: Update onConfigure**

```cpp
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
    size_t need = (size_t)nCh_ * batchSize_ * frameSize_;
    if (readBuf_.size() < need)
        readBuf_.resize(need);
    std::cout << "[Storage] Configured: " << buildDirPath()
              << " batch=" << batchSize_ << " frame=" << frameSize_
              << " maxMB=" << maxSaveSizeMB_ << " nCh=" << nCh_ << "\n";
}
```

- [ ] **Step 2: Remove `loadState` / `saveState` calls from constructor and onStart**

The old `loadState()` / `saveState()` managed session IDs via `.msm_session` file. This feature can be removed (session tracking via file count on disk is not needed for V2). In the constructor, remove the `loadState()` call. In `onStart`, remove the `saveState()` call. Delete both `loadState()` and `saveState()` methods from the cpp file and their declarations from the header.

- [ ] **Step 3: Commit**

```bash
git add module_storage/module_storage.cpp module_storage/module_storage.h
git commit -m "storage: new config fields, remove legacy session state"
```

---

### Task 4: Rewrite onStart and file open/close/rotate

**Files:**
- Modify: `module_storage/module_storage.cpp`
- Modify: `module_storage/module_storage.h`

- [ ] **Step 1: Add `slot_` to header**

```cpp
int slot_ = 0;
```

- [ ] **Step 2: Rewrite onStart**

```cpp
void ModuleStorage::onStart() {
    std::lock_guard<std::mutex> lock(fileMtx_);
    if (recording_.load()) return;
    for (int i = 0; i < nCh_; i++) {
        files_[i].framesSaved.store(0);
        files_[i].bytesSaved.store(0);
        files_[i].firstBunchID = 0;
        files_[i].lastBunchID = 0;
        files_[i].hasFirstBunch = false;
        files_[i].sequence = 0;
    }
    std::string dir = buildDirPath();
    ensureDir(dir);
    for (int i = 0; i < nCh_ && i < MAX_CH; i++) {
        auto& f = files_[i];
        f.sequence = 0;
        f.bytesWritten = 0;
        std::string path = buildTempPath(i, f.sequence);
        f.fp = fopen(path.c_str(), "wb");
        if (!f.fp)
            std::cerr << "[Storage] Failed to open " << path << ": " << strerror(errno) << "\n";
        else
            std::cout << "[Storage] " << path << "\n";
    }
    recording_ = true;
    std::cout << "[Storage] Started\n";
}
```

- [ ] **Step 3: Rewrite openFiles / closeFiles / rotateFile**

Replace `openFiles()` with logic that uses `buildTempPath`. Replace `closeFiles()` to also rename .tmp → .dat with BunchID. Replace `rotateFile()` to close → rename → open new .tmp.

```cpp
void ModuleStorage::closeFiles() {
    for (int i = 0; i < MAX_CH; i++) {
        auto& f = files_[i];
        if (!f.fp) continue;
        fclose(f.fp);
        f.fp = nullptr;
        // Rename .tmp → .dat with BunchIDs
        int ch = i;
        if (f.framesSaved.load() > 0) {
            std::string tmpPath = buildTempPath(ch, f.sequence);
            std::string finalPath = buildFinalPath(ch, f.sequence,
                f.hasFirstBunch ? f.firstBunchID : 0,
                f.lastBunchID);
            if (rename(tmpPath.c_str(), finalPath.c_str()) == 0)
                std::cout << "[Storage] -> " << finalPath << "\n";
            else
                std::cerr << "[Storage] rename failed: " << strerror(errno) << "\n";
        } else {
            // No frames written, delete empty tmp
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
    // Rename current .tmp → .dat
    std::string tmpPath = buildTempPath(ch, f.sequence);
    std::string finalPath = buildFinalPath(ch, f.sequence,
        f.hasFirstBunch ? f.firstBunchID : 0,
        f.lastBunchID);
    if (f.framesSaved.load() > 0) {
        if (rename(tmpPath.c_str(), finalPath.c_str()) == 0)
            std::cout << "[Storage] Rotate -> " << finalPath << "\n";
        else
            std::cerr << "[Storage] rename failed: " << strerror(errno) << "\n";
    } else {
        remove(tmpPath.c_str());
    }
    // Open new .tmp
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
```

- [ ] **Step 4: Update header declarations**

In `module_storage.h`, change `openFiles()` / `closeFiles()` / `rotateFile(int ch)` signatures to match the new implementation. Add `slot_` member.

- [ ] **Step 5: Commit**

```bash
git add module_storage/module_storage.h module_storage/module_storage.cpp
git commit -m "storage: rewrite onStart/closeFiles/rotateFile with new naming + BunchID rename"
```

---

### Task 5: Rewrite onIdle for batch writing + BunchID tracking

**Files:**
- Modify: `module_storage/module_storage.cpp`

- [ ] **Step 1: Rewrite onIdle**

The new logic reads per-channel batch data from saveRing, tracks BunchID, writes, and rotates at batch boundaries.

```cpp
void ModuleStorage::onIdle() {
    if (!recording_.load() && !draining_.load()) return;
    if (!saveRing_) return;

    size_t batchBytes = batchSize_ * frameSize_;
    size_t totalNeeded = (size_t)nCh_ * batchBytes;
    size_t avail = saveRing_->availableDataSize();

    if (avail < totalNeeded) {
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

    // Process each channel's batch
    for (int i = 0; i < nCh_; i++) {
        const uint8_t* chData = readBuf_.data() + i * batchBytes;
        auto& f = files_[i];
        if (!f.fp) continue;

        // Parse first frame BunchID if not yet recorded
        if (!f.hasFirstBunch) {
            f.firstBunchID = parseBunchID(chData, frameSize_);
            f.hasFirstBunch = true;
        }

        // Parse last frame BunchID from the last frame in this batch
        const uint8_t* lastFrame = chData + (batchSize_ - 1) * frameSize_;
        f.lastBunchID = parseBunchID(lastFrame, frameSize_);

        // Write batch
        size_t written = fwrite(chData, 1, batchBytes, f.fp);
        if (written != batchBytes) {
            std::cerr << "[Storage] ch" << (i + 1) << " write error: "
                      << strerror(errno) << "\n";
            continue;
        }
        f.bytesWritten += written;
        f.framesSaved.fetch_add((int64_t)batchSize_);
        f.bytesSaved.fetch_add((int64_t)written);

        // Rotate if exceeded max size
        if (maxSaveSizeMB_ > 0 &&
            f.bytesWritten >= maxSaveSizeMB_ * 1024ULL * 1024ULL) {
            std::lock_guard<std::mutex> lock(fileMtx_);
            rotateFile(i);
        }
    }
}
```

- [ ] **Step 2: Commit**

```bash
git add module_storage/module_storage.cpp
git commit -m "storage: rewrite onIdle with BunchID tracking and batch-aligned rotation"
```

---

### Task 6: Update V1 CLI main files to enable storage

**Files:**
- Modify: `module_main_v1/main_hwdaq.cpp`
- Modify: `module_main_v1/main_simsrc.cpp`
- Also verify: `module_main_v1/CMakeLists.txt` (should already link module_storage)

- [ ] **Step 1: Create saveRing and pass to ProcessFull + Storage**

In both `main_hwdaq.cpp` and `main_simsrc.cpp`:

```cpp
// After creating allRings, before ProcessFull:
UserCircularBuffer saveRing(64 * 1024 * 1024, 4096);  // 64MB shared ring
```

Pass saveRing to ProcessFull:
```cpp
ModuleProcess process("Process", allRings, &saveRing);
```

Pass saveRing + physChannels to Storage:
```cpp
std::vector<int> physCh;
for (int i = 0; i < daq.getRingCount(); i++)
    physCh.push_back(i + 1);  // or from daq_config
ModuleStorage storage(&saveRing, physCh);
```

- [ ] **Step 2: Add storage.start() after construction**

```cpp
storage.start();
```

- [ ] **Step 3: Update Config message to include FrameSize**

In `distributeConfig()`, add `FrameSize` to the Storage Configure message:

```cpp
send("Configure >> SavePath=" + g_config.data_save_path
     + ", FelName=" + g_config.fel_name
     + ", SystemName=" + g_config.system_name
     + ", DeviceName=" + g_config.device_name
     + ", ExpNo=" + std::to_string(g_config.exp_no)
     + ", RunSerial=" + g_config.run_serial
     + ", BatchSize=10000"
     + ", FrameSize=1152"  // or read from config
     + ", MaxSaveSizeMB=" + std::to_string(g_config.max_save_size_mb)
     + " -@ Storage");
```

Note: check if `DaqConfig` has these fields. If not, add them to `daq_config.h` / `.cpp` with defaults.

- [ ] **Step 4: Wire Start/Stop ordering for Storage**

In the `start` command handler:
```cpp
send("Start >> -@ Storage");         // 1st
std::this_thread::sleep_for(std::chrono::milliseconds(100));
send("Start >> -@ Process");         // 2nd
std::this_thread::sleep_for(std::chrono::milliseconds(100));
send("Start >> -@ DAQModule");       // 3rd
```

In the `stop` command handler:
```cpp
send("Stop >> -@ DAQModule");        // 1st
std::this_thread::sleep_for(std::chrono::milliseconds(100));
send("Stop >> -@ Process");          // 2nd
std::this_thread::sleep_for(std::chrono::milliseconds(100));
send("Stop >> -@ Storage");          // 3rd
```

Same ordering in the shutdown sequence at end of `main()`.

- [ ] **Step 5: Commit**

```bash
git add module_main_v1/main_hwdaq.cpp module_main_v1/main_simsrc.cpp
git commit -m "v1: enable storage — create saveRing, connect Process+Storage, wire Start/Stop ordering"
```

---

### Task 7: Add missing fields to DaqConfig

**Files:**
- Modify: `module_daq/daq_config.h`
- Modify: `module_daq/daq_config.cpp`

- [ ] **Step 1: Add fields to DaqConfig**

In `module_daq/daq_config.h`, add:
```cpp
std::string fel_name = "fel1";
std::string system_name = "HSS";
std::string device_name = "PD";
int exp_no = 1;
std::string run_serial = "r00001";
int slot = 0;
size_t frame_size = 1152;
```

In `module_daq/daq_config.cpp`, load these from JSON config and include in `toKeyValues()`.

- [ ] **Step 2: Commit**

```bash
git add module_daq/daq_config.h module_daq/daq_config.cpp
git commit -m "daq_config: add storage-related fields (felName, systemName, deviceName, expNo, runSerial)"
```

---

### Task 8: Build and test with simsource

**Files:**
- Test: run `msm_cli_simsrc` + `msm_ipc_shell`

- [ ] **Step 1: Build**

```bash
cmake -B build && cmake --build build
```
Expected: all targets build clean.

- [ ] **Step 2: Run simsource, start acquisition, verify files appear**

```bash
rm -rf /tmp/test_storage
mkdir -p /tmp/test_storage
# (First update config to use /tmp/test_storage, then:)
./build/msm_cli_simsrc
# In another terminal:
#   echo "start" | ./build/msm_ipc_shell
#   sleep 2
#   echo "status" | ./build/msm_ipc_shell
#   echo "stop" | ./build/msm_ipc_shell
# Check /tmp/test_storage/fel1/HSS/PD/*/*/card*/
```

- [ ] **Step 3: Verify file naming and content**

Check:
- Directory hierarchy matches spec
- .tmp files exist during acquisition, renamed to .dat on stop
- Filenames contain correct EXP number, channel, BunchID, seq
- BunchID in filename matches actual frame data

- [ ] **Step 4: Commit**

```bash
git add -A && git commit -m "v2: storage save-file feature complete"
```

---

### Self-Review Checklist

1. **Spec coverage:** Every section in the spec (data flow, lifecycle, naming, rotation, BunchID, status, config) is covered by at least one task.
2. **Placeholder check:** No TBD/TODO — all code blocks contain real implementations.
3. **Type consistency:** `parseBunchID` returns `uint64_t`, matching `firstBunchID`/`lastBunchID` types. `buildTempPath` and `buildFinalPath` use the same parameters.
4. **DaqConfig fields:** Task 7 adds the needed fields; if those already exist, that task can be skipped.
