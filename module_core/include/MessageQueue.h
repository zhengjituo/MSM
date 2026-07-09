#ifndef MSM_MESSAGE_QUEUE_H
#define MSM_MESSAGE_QUEUE_H

#include "Message.h"
#include <deque>
#include <mutex>
#include <atomic>
#include <condition_variable>

namespace msm {

enum class MessagePriority {
    LOW = 0,
    HIGH = 1,
    HIGHEST = 2
};

class MessageQueue {
public:
    MessageQueue(size_t maxSize = 1000);
    ~MessageQueue();
    
    MessageQueue(const MessageQueue&) = delete;
    MessageQueue& operator=(const MessageQueue&) = delete;
    
    bool push(const Message& msg, MessagePriority priority = MessagePriority::LOW);
    Message pop();
    Message peek() const;
    bool empty() const;
    size_t size() const;
    void stop();
    bool isStopped() const;
    bool waitMessage(int timeout_ms = -1);
    
private:
    std::deque<Message> queue_low_;
    std::deque<Message> queue_high_;
    std::deque<Message> queue_highest_;
    
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> stopped_;
    size_t maxSize_;
};

}

#endif