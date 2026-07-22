#ifndef USER_CIRCULAR_BUFFER_H
#define USER_CIRCULAR_BUFFER_H

#include <cstdint>
#include <cstddef>
#include <atomic>

class UserCircularBuffer {
public:
    explicit UserCircularBuffer(size_t total_size, size_t expected_packet_size = 8320);
    ~UserCircularBuffer();

    size_t writeFromDevice(const void* data, size_t size);
    size_t readData(void* dest, size_t size);
    size_t peekData(void* dest, size_t size) const;
    size_t availableDataSize() const;
    size_t availableFrames() const;
    size_t availableSpace() const;

    void resetCounters() {
        write_total_.store(0, std::memory_order_relaxed);
        read_total_.store(0, std::memory_order_relaxed);
        overflow_count_.store(0, std::memory_order_relaxed);
    }
    bool hasOverflow() const { return overflow_count_.load(std::memory_order_relaxed) > 0; }
    size_t getOverflowCount() const { return overflow_count_.load(std::memory_order_relaxed); }
    void resetOverflow() { overflow_count_.store(0, std::memory_order_relaxed); }
    uint64_t getWriteTotal() const { return write_total_.load(std::memory_order_relaxed); }
    uint64_t getReadTotal() const { return read_total_.load(std::memory_order_relaxed); }
    size_t getCapacity() const { return capacity_; }
    size_t getAlignCapacity() const { return align_capacity_; }

    size_t getExpectedPacketSize() const { return expected_packet_size_; }
    void setExpectedPacketSize(size_t packet_size);

    static size_t calculatePacketSize(uint32_t trig_length_ns, uint16_t sample_rate_mhz) {
        uint64_t raw_points = static_cast<uint64_t>(trig_length_ns) * sample_rate_mhz / 1000;
        uint64_t aligned_points = ((raw_points + 31) / 32) * 32;
        return 64 + static_cast<size_t>(aligned_points * 2) + 64;
    }

    static uint32_t parsePacketLength(const void* data) {
        if (!data) return 0;
        const uint8_t* ptr = static_cast<const uint8_t*>(data);
        if (ptr[0] != 0xEB || ptr[1] != 0x91) return 0;
        return (static_cast<uint32_t>(ptr[2]) << 24) |
               (static_cast<uint32_t>(ptr[3]) << 16) |
               (static_cast<uint32_t>(ptr[4]) << 8) |
               static_cast<uint32_t>(ptr[5]);
    }

private:
    UserCircularBuffer(const UserCircularBuffer&) = delete;
    UserCircularBuffer& operator=(const UserCircularBuffer&) = delete;

    void recalcAlignCapacity();

    uint8_t* buffer_;
    size_t capacity_;
    size_t align_capacity_;
    size_t expected_packet_size_;

    // 单调递增 64 位计数器，永不回绕
    std::atomic<uint64_t> write_total_{0};
    std::atomic<uint64_t> read_total_{0};
    std::atomic<size_t> overflow_count_{0};
};

#endif
