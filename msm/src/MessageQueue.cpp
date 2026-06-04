#include "MessageQueue.h"
#include <algorithm>

namespace msm {

MessageQueue::MessageQueue(size_t maxSize)
    : stopped_(false), maxSize_(maxSize) {}

MessageQueue::~MessageQueue() {
    stop();
}

bool MessageQueue::push(const Message& msg, MessagePriority priority) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (stopped_) {
        return false;
    }
    
    size_t totalSize = queue_low_.size() + queue_high_.size() + queue_highest_.size();
    if (totalSize >= maxSize_) {
        return false;
    }
    
    switch (priority) {
        case MessagePriority::HIGHEST:
            queue_highest_.push_back(msg);
            break;
        case MessagePriority::HIGH:
            queue_high_.push_back(msg);
            break;
        case MessagePriority::LOW:
        default:
            queue_low_.push_back(msg);
            break;
    }
    
    cv_.notify_one();
    return true;
}

Message MessageQueue::pop() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!queue_highest_.empty()) {
        Message msg = queue_highest_.front();
        queue_highest_.pop_front();
        return msg;
    }
    
    if (!queue_high_.empty()) {
        Message msg = queue_high_.front();
        queue_high_.pop_front();
        return msg;
    }
    
    if (!queue_low_.empty()) {
        Message msg = queue_low_.front();
        queue_low_.pop_front();
        return msg;
    }
    
    return Message{};
}

Message MessageQueue::peek() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!queue_highest_.empty()) {
        return queue_highest_.front();
    }
    
    if (!queue_high_.empty()) {
        return queue_high_.front();
    }
    
    if (!queue_low_.empty()) {
        return queue_low_.front();
    }
    
    return Message{};
}

bool MessageQueue::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_low_.empty() && queue_high_.empty() && queue_highest_.empty();
}

size_t MessageQueue::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_low_.size() + queue_high_.size() + queue_highest_.size();
}

void MessageQueue::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_.store(true, std::memory_order_release);
    cv_.notify_all();
}

bool MessageQueue::isStopped() const {
    return stopped_.load(std::memory_order_acquire);
}

bool MessageQueue::waitMessage(int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    
    if (!queue_highest_.empty() || !queue_high_.empty() || !queue_low_.empty()) {
        return true;
    }
    
    if (stopped_) {
        return false;
    }
    
    if (timeout_ms < 0) {
        cv_.wait(lock, [this] {
            return !queue_highest_.empty() || !queue_high_.empty() || 
                   !queue_low_.empty() || stopped_;
        });
    } else {
        auto timeout = std::chrono::milliseconds(timeout_ms);
        bool ret = cv_.wait_for(lock, timeout, [this] {
            return !queue_highest_.empty() || !queue_high_.empty() || 
                   !queue_low_.empty() || stopped_;
        });
        if (!ret) {
            return false;
        }
    }
    
    return !stopped_;
}

} // namespace msm