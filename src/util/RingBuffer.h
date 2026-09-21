#pragma once
// Single-producer / single-consumer lock-free byte ring buffer.
// Used for MIDI events, atom messages and worker traffic between threads.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

class RingBuffer {
public:
    explicit RingBuffer(size_t capacity = 65536) : buf_(roundUp(capacity)), mask_(buf_.size() - 1) {}

    size_t capacity() const { return buf_.size(); }

    size_t readAvailable() const {
        return (write_.load(std::memory_order_acquire) - read_.load(std::memory_order_relaxed));
    }
    size_t writeAvailable() const {
        return buf_.size() - (write_.load(std::memory_order_relaxed) - read_.load(std::memory_order_acquire));
    }

    bool write(const void* data, size_t n) {
        if (n > writeAvailable()) return false;
        size_t w = write_.load(std::memory_order_relaxed);
        const uint8_t* src = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < n; ++i) buf_[(w + i) & mask_] = src[i];
        write_.store(w + n, std::memory_order_release);
        return true;
    }
    bool read(void* data, size_t n) {
        if (n > readAvailable()) return false;
        size_t r = read_.load(std::memory_order_relaxed);
        uint8_t* dst = static_cast<uint8_t*>(data);
        for (size_t i = 0; i < n; ++i) dst[i] = buf_[(r + i) & mask_];
        read_.store(r + n, std::memory_order_release);
        return true;
    }
    bool peek(void* data, size_t n) const {
        if (n > readAvailable()) return false;
        size_t r = read_.load(std::memory_order_relaxed);
        uint8_t* dst = static_cast<uint8_t*>(data);
        for (size_t i = 0; i < n; ++i) dst[i] = buf_[(r + i) & mask_];
        return true;
    }
    void skip(size_t n) { read_.store(read_.load(std::memory_order_relaxed) + n, std::memory_order_release); }

    // Convenience for length-prefixed messages: [uint32 size][payload]
    bool writeMessage(const void* data, uint32_t n) {
        if (sizeof(uint32_t) + n > writeAvailable()) return false;
        write(&n, sizeof(n));
        write(data, n);
        return true;
    }
    // Returns payload size or 0 if none. Copies into out (must be >= maxOut).
    uint32_t readMessage(void* out, uint32_t maxOut) {
        uint32_t n = 0;
        if (!peek(&n, sizeof(n))) return 0;
        if (n > maxOut) { // discard oversized message
            skip(sizeof(n) + n);
            return 0;
        }
        skip(sizeof(n));
        read(out, n);
        return n;
    }

private:
    static size_t roundUp(size_t v) {
        size_t p = 1;
        while (p < v) p <<= 1;
        return p;
    }
    std::vector<uint8_t> buf_;
    size_t mask_;
    std::atomic<size_t> read_{0};
    std::atomic<size_t> write_{0};
};
