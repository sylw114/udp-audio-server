#pragma once
// ============================================================================
// ring_buffer.h — Lock-Free SPSC (Single Producer Single Consumer) Ring Buffer
// 
// 网络线程（生产者）写入 Float32 音频帧
// WASAPI 渲染线程（消费者）读取 Float32 音频帧
// 无锁设计，使用 std::atomic acquire/release 语义
// ============================================================================

#include <atomic>
#include <cstring>
#include <algorithm>

class RingBuffer {
public:
    // capacity: 环形缓冲区容量，单位为 float 样本数（frames * channels）
    // 强制调整为 2 的幂以支持高效的掩码操作
    RingBuffer(size_t capacity_samples)
    {
        capacity_ = nextPowerOf2(capacity_samples);
        mask_ = capacity_ - 1;
        buffer_ = new float[capacity_];
        memset(buffer_, 0, capacity_ * sizeof(float));
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }

    ~RingBuffer() {
        delete[] buffer_;
    }

    // 禁止拷贝
    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    // 生产者调用：写入 count 个 float 样本
    // 返回实际写入的样本数
    size_t write(const float* data, size_t count) {
        const size_t current_head = head_.load(std::memory_order_relaxed);
        const size_t current_tail = tail_.load(std::memory_order_acquire);

        const size_t available = capacity_ - (current_head - current_tail);
        const size_t to_write = std::min(count, available);

        if (to_write == 0) return 0;

        const size_t head_index = current_head & mask_;
        const size_t first_chunk = std::min(to_write, capacity_ - head_index);
        const size_t second_chunk = to_write - first_chunk;

        memcpy(buffer_ + head_index, data, first_chunk * sizeof(float));
        if (second_chunk > 0) {
            memcpy(buffer_, data + first_chunk, second_chunk * sizeof(float));
        }

        head_.store(current_head + to_write, std::memory_order_release);
        return to_write;
    }

    // 消费者调用：读取 count 个 float 样本
    // 返回实际读取的样本数
    size_t read(float* data, size_t count) {
        const size_t current_tail = tail_.load(std::memory_order_relaxed);
        const size_t current_head = head_.load(std::memory_order_acquire);

        const size_t available = current_head - current_tail;
        const size_t to_read = std::min(count, available);

        if (to_read == 0) return 0;

        const size_t tail_index = current_tail & mask_;
        const size_t first_chunk = std::min(to_read, capacity_ - tail_index);
        const size_t second_chunk = to_read - first_chunk;

        memcpy(data, buffer_ + tail_index, first_chunk * sizeof(float));
        if (second_chunk > 0) {
            memcpy(data + first_chunk, buffer_, second_chunk * sizeof(float));
        }

        tail_.store(current_tail + to_read, std::memory_order_release);
        return to_read;
    }

    // 查询当前可读样本数
    size_t availableToRead() const {
        const size_t current_head = head_.load(std::memory_order_acquire);
        const size_t current_tail = tail_.load(std::memory_order_relaxed);
        return current_head - current_tail;
    }

    // 重置缓冲区
    void reset() {
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }

private:
    static size_t nextPowerOf2(size_t n) {
        if (n == 0) return 1;
        n--;
        n |= n >> 1;
        n |= n >> 2;
        n |= n >> 4;
        n |= n >> 8;
        n |= n >> 16;
        if (sizeof(size_t) > 4) n |= n >> 32;
        return n + 1;
    }

    float*              buffer_;
    size_t              capacity_;
    size_t              mask_;
    // 使用 alignas 避免 false sharing
    alignas(64) std::atomic<size_t> head_;
    alignas(64) std::atomic<size_t> tail_;
};
