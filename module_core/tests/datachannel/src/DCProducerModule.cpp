#include "DCProducerModule.h"
#include <iostream>
#include <cstring>
#include <chrono>

namespace msm {

DCProducerModule::DCProducerModule(const std::string& name,
                                   const std::string& peerName,
                                   const std::string& targetModule,
                                   int framesPerBatch, int frameSize,
                                   long totalBatches, DataChannel* dc)
    : Module(name), peerName_(peerName), targetModule_(targetModule), dc_(dc)
{
    data_.frames_per_batch = framesPerBatch;
    data_.frame_size = frameSize;
    data_.total_batches = totalBatches;
}

void DCProducerModule::handleState(const Message& msg) {
    if (msg.cmd == "START") {
        if (started_) return;
        started_ = true;
        done_ = false;
        batchesSent_ = 0;
        bytesSent_ = 0;
        tStart_ = std::chrono::steady_clock::now();

        size_t batchBytes = (size_t)data_.frame_size * data_.frames_per_batch;
        std::vector<char> buf(batchBytes);
        // Pre-fill once with pattern
        {
            double* dp = reinterpret_cast<double*>(buf.data());
            for (size_t i = 0; i < batchBytes / sizeof(double); i++) {
                dp[i] = (double)i;
            }
        }

        std::cout << "[" << name_ << "] Sending " << data_.total_batches
                  << " batches x " << data_.frames_per_batch << " frames ("
                  << batchBytes << " bytes/batch, "
                  << (batchBytes * data_.total_batches / 1024.0 / 1024.0 / 1024.0) << " GB)\n";

        for (long b = 0; b < data_.total_batches; b++) {
            // Stamp batch ID into first 8 bytes only
            double* dp = reinterpret_cast<double*>(buf.data());
            *dp = (double)(b * 1000000);

            // Send DATA_READY FIRST so consumer starts reading TCP
            Message notify;
            notify.cmd = "DATA_READY";
            notify.data.set("byte_count", (int)batchBytes);
            notify.data.set("frame_size", data_.frame_size);
            notify.data.set("frames", data_.frames_per_batch);
            notify.setTarget(targetModule_);
            notify.setRemote(peerName_);
            MessageBus::instance().send(notify);

            // 发送前检查 TCP 连接健康状态
            if (!dc_->isHealthy()) {
                std::cerr << "[" << name_ << "] TCP unhealthy before batch " << b
                          << ", reconnecting...\n";
                if (!dc_->reconnect()) {
                    std::cerr << "[" << name_ << "] Reconnect failed, aborting\n";
                    break;
                }
                std::cout << "[" << name_ << "] Reconnected OK\n";
            }

            // TCP send (超时 5s, 失败则有重连)
            if (!dc_->send(buf.data(), batchBytes)) {
                std::cerr << "[" << name_ << "] TCP send failed at batch " << b << "\n";
                std::cout << "[" << name_ << "] Attempting reconnect...\n";
                if (dc_->reconnect()) {
                    std::cout << "[" << name_ << "] Reconnected, retrying batch " << b << "\n";
                    if (!dc_->send(buf.data(), batchBytes)) {
                        std::cerr << "[" << name_ << "] Retry failed, aborting\n";
                        break;
                    }
                } else {
                    std::cerr << "[" << name_ << "] Reconnect failed, aborting\n";
                    break;
                }
            }
            bytesSent_ += batchBytes;
            batchesSent_++;
        }

        auto tEnd = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(tEnd - tStart_).count();
        data_.throughput_mbs = bytesSent_ / 1024.0 / 1024.0 / elapsed;
        data_.batches_sent = batchesSent_;
        data_.bytes_sent = bytesSent_;
        data_.done = 1;
        done_ = true;

        std::cout << "\n[" << name_ << "] Done. " << bytesSent_ << " bytes in "
                  << elapsed << "s (" << data_.throughput_mbs << " MB/s)\n";

        Message done;
        done.cmd = "DONE";
        done.data.set("total_batches", (int)data_.total_batches);
        done.data.set("total_bytes", (int)bytesSent_);
        done.setTarget(targetModule_);
        done.setRemote(peerName_);
        MessageBus::instance().send(done);

        // 验证 PUB/SUB 广播
        Message pubTest;
        pubTest.cmd = "BROADCAST_TEST";
        pubTest.data.set("msg", "PUB/SUB cross-network OK");
        pubTest.data.set("module", "DCProducer");
        pubTest.data.set("var", "test");
        pubTest.data.set("value", std::to_string(bytesSent_));
        MessageBus::instance().broadcast(pubTest);
        std::cout << "[" << name_ << "] Broadcast sent\n";
    }
}

ModuleVars* DCProducerModule::getVars() {
    struct Vars : public ModuleVars {
        DCProducerModule* mod;
        Vars(DCProducerModule* m) : mod(m) {}
        std::map<std::string, std::string> list() override {
            return {
                {"action", std::to_string(mod->data_.action)},
                {"frames_per_batch", std::to_string(mod->data_.frames_per_batch)},
                {"frame_size", std::to_string(mod->data_.frame_size)},
                {"total_batches", std::to_string(mod->data_.total_batches)},
                {"batches_sent", std::to_string(mod->data_.batches_sent)},
                {"bytes_sent", std::to_string(mod->data_.bytes_sent)},
                {"throughput_mbs", std::to_string(mod->data_.throughput_mbs)},
                {"done", std::to_string(mod->data_.done)},
            };
        }
        void* get(const std::string& key) override {
            if (key == "action") return &mod->data_.action;
            if (key == "done") return &mod->data_.done;
            return nullptr;
        }
        bool set(const std::string& key, const std::string& val) override {
            if (key == "action") {
                mod->data_.action = std::stoi(val);
                if (mod->data_.action == 1) {
                    mod->handleState(Message{"START"});
                }
                return true;
            }
            return false;
        }
    };
    static Vars v(this);
    return &v;
}

} // namespace msm
