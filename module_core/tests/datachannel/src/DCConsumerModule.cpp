#include "DCConsumerModule.h"
#include <iostream>
#include <cstring>
#include <chrono>

namespace msm {

DCConsumerModule::DCConsumerModule(const std::string& name, DataChannel* dc)
    : Module(name), dc_(dc)
{
    tStart_ = std::chrono::steady_clock::now();
}

void DCConsumerModule::handleState(const Message& msg) {
    if (msg.cmd == "DATA_READY") {
        size_t byteCount = std::stoul(msg.data.get("byte_count", "0"));
        if (byteCount == 0) return;

        // Read from TCP data channel (reuse pre-allocated buffer)
        if (buf_.size() < byteCount)
            buf_.resize(byteCount);
        if (!dc_->recvAll(buf_.data(), byteCount)) {
            std::cerr << "[" << name_ << "] TCP recv failed, reconnecting...\n";
            data_.errors++;
            if (dc_->reconnect()) {
                std::cout << "[" << name_ << "] Reconnected OK\n";
            } else {
                std::cerr << "[" << name_ << "] Reconnect failed\n";
            }
            return;
        }

        bytesReceived_ += byteCount;
        batchesReceived_++;

        // Quick validation: first 8 bytes should be batch identifier
        double* dp = reinterpret_cast<double*>(buf_.data());
        long batchId = (long)(dp[0] / 1000000.0);
        (void)batchId;  // available for validation if needed

        data_.bytes_received = bytesReceived_;
        data_.batches_received = batchesReceived_;

        if (batchesReceived_ % 1000 == 0) {
            auto now = std::chrono::steady_clock::now();
            double sec = std::chrono::duration<double>(now - tStart_).count();
            double mbps = bytesReceived_ / 1024.0 / 1024.0 / sec;
            std::cout << "\r[" << name_ << "] " << batchesReceived_ << " batches, "
                      << bytesReceived_/1024/1024 << " MB, "
                      << mbps << " MB/s   ";
            std::cout.flush();
        }
    } else if (msg.cmd == "DONE") {
        long totalBatches = std::stol(msg.data.get("total_batches", "0"));
        long totalBytes = std::stol(msg.data.get("total_bytes", "0"));
        (void)totalBatches; (void)totalBytes;
        done_ = true;
        data_.done = 1;

        auto tEnd = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(tEnd - tStart_).count();
        data_.throughput_mbs = bytesReceived_ / 1024.0 / 1024.0 / elapsed;

        std::cout << "\n[" << name_ << "] Done. " << bytesReceived_
                  << " bytes in " << elapsed << "s ("
                  << data_.throughput_mbs << " MB/s), "
                  << data_.errors << " errors\n";
    }
}

ModuleVars* DCConsumerModule::getVars() {
    struct Vars : public ModuleVars {
        DCConsumerModule* mod;
        Vars(DCConsumerModule* m) : mod(m) {}
        std::map<std::string, std::string> list() override {
            return {
                {"batches_received", std::to_string(mod->data_.batches_received)},
                {"bytes_received", std::to_string(mod->data_.bytes_received)},
                {"throughput_mbs", std::to_string(mod->data_.throughput_mbs)},
                {"errors", std::to_string(mod->data_.errors)},
                {"done", std::to_string(mod->data_.done)},
            };
        }
        void* get(const std::string& key) override {
            if (key == "done") return &mod->data_.done;
            return nullptr;
        }
        bool set(const std::string&, const std::string&) override { return false; }
    };
    static Vars v(this);
    return &v;
}

} // namespace msm
