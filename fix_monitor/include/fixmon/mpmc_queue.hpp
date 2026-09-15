#pragma once
//
// Bounded MPMC queue (Vyukov). Many adapter threads produce, one pipeline
// thread consumes. Bounded on purpose: if the consumer falls behind we want to
// drop and count the drop, not grow without limit and OOM the box that is also
// running the FIX engine.
//
#include <atomic>
#include <cstddef>
#include <memory>
#include <new>

namespace fixmon {

// Fixed at 64 rather than std::hardware_destructive_interference_size: that
// constant is not ABI-stable across translation units on GCC and warns. Every
// target this runs on has a 64-byte line.
inline constexpr size_t kCacheLine = 64;

template <typename T>
class MpmcQueue {
public:
    // capacity must be a power of two and >= 2.
    explicit MpmcQueue(size_t capacity)
        : buffer_(new Cell[capacity]), mask_(capacity - 1) {
        for (size_t i = 0; i < capacity; ++i) {
            buffer_[i].seq.store(i, std::memory_order_relaxed);
        }
        enqueue_pos_.store(0, std::memory_order_relaxed);
        dequeue_pos_.store(0, std::memory_order_relaxed);
    }

    MpmcQueue(const MpmcQueue&)            = delete;
    MpmcQueue& operator=(const MpmcQueue&) = delete;

    bool try_push(T&& value) {
        Cell*  cell;
        size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            cell            = &buffer_[pos & mask_];
            size_t    seq   = cell->seq.load(std::memory_order_acquire);
            intptr_t  diff  = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
            if (diff == 0) {
                if (enqueue_pos_.compare_exchange_weak(
                        pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false;  // full
            } else {
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }
        cell->data = std::move(value);
        cell->seq.store(pos + 1, std::memory_order_release);
        return true;
    }

    bool try_pop(T& out) {
        Cell*  cell;
        size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            cell           = &buffer_[pos & mask_];
            size_t   seq   = cell->seq.load(std::memory_order_acquire);
            intptr_t diff  = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
            if (diff == 0) {
                if (dequeue_pos_.compare_exchange_weak(
                        pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false;  // empty
            } else {
                pos = dequeue_pos_.load(std::memory_order_relaxed);
            }
        }
        out = std::move(cell->data);
        cell->seq.store(pos + mask_ + 1, std::memory_order_release);
        return true;
    }

    size_t size_approx() const {
        size_t e = enqueue_pos_.load(std::memory_order_relaxed);
        size_t d = dequeue_pos_.load(std::memory_order_relaxed);
        return e > d ? e - d : 0;
    }

    size_t capacity() const { return mask_ + 1; }

private:
    struct Cell {
        std::atomic<size_t> seq;
        T                   data;
    };

    std::unique_ptr<Cell[]> buffer_;
    const size_t            mask_;

    alignas(kCacheLine) std::atomic<size_t> enqueue_pos_;
    alignas(kCacheLine) std::atomic<size_t> dequeue_pos_;
    char pad_[kCacheLine];
};

}  // namespace fixmon
