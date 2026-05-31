#pragma once
#include <vector>
#include <atomic>
#include <cstdint>

struct AudioPacket {
    uint8_t seq;
    std::vector<uint8_t> data;
};

// Simple lock-free SPSC queue for packets
class PacketQueue {
public:
    PacketQueue(size_t capacity) : capacity_(capacity), head_(0), tail_(0) {
        queue_ = new AudioPacket*[capacity_];
        for (size_t i = 0; i < capacity_; ++i) queue_[i] = nullptr;
    }
    ~PacketQueue() {
        for (size_t i = 0; i < capacity_; ++i) delete queue_[i];
        delete[] queue_;
    }

    bool push(AudioPacket* packet) {
        size_t t = tail_.load(std::memory_order_relaxed);
        size_t next_t = (t + 1) % capacity_;
        if (next_t == head_.load(std::memory_order_acquire)) return false;
        queue_[t] = packet;
        tail_.store(next_t, std::memory_order_release);
        return true;
    }

    AudioPacket* pop() {
        size_t h = head_.load(std::memory_order_relaxed);
        if (h == tail_.load(std::memory_order_acquire)) return nullptr;
        AudioPacket* packet = queue_[h];
        queue_[h] = nullptr;
        head_.store((h + 1) % capacity_, std::memory_order_release);
        return packet;
    }

private:
    size_t capacity_;
    AudioPacket** queue_;
    std::atomic<size_t> head_, tail_;
};
