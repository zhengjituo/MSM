#include "Module.h"

namespace msm {

Module::Module(const std::string& name)
    : name_(name), running_(false) {
}

Module::~Module() {
    stop();
}

void Module::start() {
    if (running_) return;
    running_ = true;
    thread_ = std::make_unique<std::thread>(&Module::runLoop, this);
}

void Module::stop() {
    if (!running_) return;
    running_ = false;
    queue_.push(Message{});
    if (thread_ && thread_->joinable()) {
        thread_->join();
        thread_.reset();
    }
}

void Module::post(const Message& msg) {
    std::lock_guard<std::mutex> lock(mtx_);
    queue_.push(msg);
}

void Module::runLoop() {
    while (running_) {
        Message msg;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            if (!queue_.empty()) {
                msg = queue_.front();
                queue_.pop();
            }
        }
        
        if (!msg.cmd.empty()) {
            // 队列有消息则处理
            handleState(msg);
        } else {
            // 队列无消息则处理空消息帧（IDLE）
            handleState(Message{});
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    running_ = false;
}

} // namespace msm