#include "user_circular_buffer.h"
#include <cstring>
#include <algorithm>
#include <iostream>

UserCircularBuffer::UserCircularBuffer(size_t total_size, size_t expected_packet_size)
    : capacity_(total_size),
      expected_packet_size_(expected_packet_size)
{
    buffer_ = new uint8_t[capacity_];
    std::memset(buffer_, 0, capacity_);
    recalcAlignCapacity();

    std::cout << "[UserCircularBuffer] Initialized with:" << std::endl;
    std::cout << "  - Total capacity: " << capacity_ << " bytes ("
              << (capacity_ / 1024 / 1024) << " MB)" << std::endl;
    std::cout << "  - Expected packet size: " << expected_packet_size_ << " bytes" << std::endl;
    std::cout << "  - Estimated max packets: " << (align_capacity_ / expected_packet_size_) << std::endl;
}

UserCircularBuffer::~UserCircularBuffer() {
    delete[] buffer_;
}

void UserCircularBuffer::recalcAlignCapacity() {
    if (expected_packet_size_ == 0) {
        align_capacity_ = capacity_;
        return;
    }
    align_capacity_ = (capacity_ / expected_packet_size_) * expected_packet_size_;
}

void UserCircularBuffer::setExpectedPacketSize(size_t packet_size) {
    expected_packet_size_ = packet_size;
    recalcAlignCapacity();
}

size_t UserCircularBuffer::writeFromDevice(const void* data, size_t size) {
    if (!data || size == 0) return 0;
    if (align_capacity_ == 0) return 0;

    uint64_t cur_write = write_total_.load(std::memory_order_relaxed);
    uint64_t cur_read  = read_total_.load(std::memory_order_acquire);

    uint64_t avail = cur_write - cur_read;
    if (avail + size > align_capacity_) {
        overflow_count_.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }

    const uint8_t* src = static_cast<const uint8_t*>(data);
    size_t offset = cur_write % align_capacity_;
    size_t first = std::min(size, align_capacity_ - offset);
    std::memcpy(buffer_ + offset, src, first);
    if (first < size)
        std::memcpy(buffer_, src + first, size - first);

    write_total_.store(cur_write + size, std::memory_order_release);
    return size;
}

size_t UserCircularBuffer::peekAt(uint64_t off, void* dest, size_t size) const {
    if (!dest || size == 0) return 0;
    if (align_capacity_ == 0) return 0;

    uint64_t cur_read  = read_total_.load(std::memory_order_relaxed);
    uint64_t cur_write = write_total_.load(std::memory_order_acquire);
    uint64_t avail = cur_write - cur_read;
    if (off + size > avail) return 0;   // 数据未就位，调用方应先查 available

    uint8_t* dst = static_cast<uint8_t*>(dest);
    uint64_t pos = (cur_read + off) % align_capacity_;
    size_t first = std::min(size, (size_t)(align_capacity_ - pos));
    std::memcpy(dst, buffer_ + pos, first);
    if (first < size)
        std::memcpy(dst + first, buffer_, size - first);
    return size;
}

bool UserCircularBuffer::skip(size_t n) {
    if (n == 0) return true;
    if (align_capacity_ == 0) return false;

    // 单消费端（process）调用，与 readData 同风格：先校验再推进
    uint64_t cur_read  = read_total_.load(std::memory_order_relaxed);
    uint64_t cur_write = write_total_.load(std::memory_order_acquire);
    if (n > cur_write - cur_read) return false;
    read_total_.store(cur_read + n, std::memory_order_release);
    return true;
}

size_t UserCircularBuffer::readData(void* dest, size_t size) {
    if (!dest || size == 0) return 0;
    if (align_capacity_ == 0) return 0;

    uint64_t cur_read  = read_total_.load(std::memory_order_relaxed);
    uint64_t cur_write = write_total_.load(std::memory_order_acquire);

    uint64_t avail = cur_write - cur_read;
    if (avail == 0) return 0;

    uint64_t to_read = std::min((uint64_t)size, avail);
    uint8_t* dst = static_cast<uint8_t*>(dest);

    size_t offset = cur_read % align_capacity_;
    size_t first = std::min((size_t)to_read, align_capacity_ - offset);
    std::memcpy(dst, buffer_ + offset, first);
    if (first < to_read)
        std::memcpy(dst + first, buffer_, (size_t)(to_read - first));

    read_total_.store(cur_read + to_read, std::memory_order_release);
    return (size_t)to_read;
}

size_t UserCircularBuffer::peekData(void* dest, size_t size) const {
    if (!dest || size == 0) return 0;
    if (align_capacity_ == 0) return 0;

    uint64_t cur_read  = read_total_.load(std::memory_order_relaxed);
    uint64_t cur_write = write_total_.load(std::memory_order_acquire);

    uint64_t avail = cur_write - cur_read;
    if (avail == 0) return 0;

    uint64_t to_read = std::min((uint64_t)size, avail);
    uint8_t* dst = static_cast<uint8_t*>(dest);

    size_t offset = cur_read % align_capacity_;
    size_t first = std::min((size_t)to_read, align_capacity_ - offset);
    std::memcpy(dst, buffer_ + offset, first);
    if (first < to_read)
        std::memcpy(dst + first, buffer_, (size_t)(to_read - first));

    return (size_t)to_read;
}

size_t UserCircularBuffer::availableDataSize() const {
    uint64_t w = write_total_.load(std::memory_order_acquire);
    uint64_t r = read_total_.load(std::memory_order_acquire);
    return (size_t)(w - r);
}

size_t UserCircularBuffer::availableFrames() const {
    if (expected_packet_size_ == 0) return 0;
    return availableDataSize() / expected_packet_size_;
}

size_t UserCircularBuffer::availableSpace() const {
    uint64_t w = write_total_.load(std::memory_order_acquire);
    uint64_t r = read_total_.load(std::memory_order_acquire);
    uint64_t used = w - r;
    return capacity_ - (used > capacity_ ? capacity_ : (size_t)used);
}
