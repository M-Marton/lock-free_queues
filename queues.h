#ifndef QUEUES_H
#define QUEUES_H

#include <atomic>
#include <queue>
#include <mutex>
#include <vector>
#include <cstdint>
#include <stdexcept>
#include <memory>
#include <thread>

// ============================================================================
// 1. MUTEX QUEUE (Baseline - Blocking)
// ============================================================================
template<typename T>
class MutexQueue {
private:
    std::queue<T> q;
    mutable std::mutex mtx;
    size_t cap;
    std::atomic<size_t> sz{0};
    
public:
    explicit MutexQueue(size_t capacity) : cap(capacity) {
        if (capacity == 0) throw std::invalid_argument("Capacity must be positive");
    }
    
    bool enqueue(T item) {
        std::lock_guard<std::mutex> lock(mtx);
        if (q.size() >= cap) return false;
        q.push(std::move(item));
        sz.store(q.size(), std::memory_order_release);
        return true;
    }
    
    bool dequeue(T& item) {
        std::lock_guard<std::mutex> lock(mtx);
        if (q.empty()) return false;
        item = std::move(q.front());
        q.pop();
        sz.store(q.size(), std::memory_order_release);
        return true;
    }
    
    size_t size() const { return sz.load(std::memory_order_acquire); }
    bool empty() const { return size() == 0; }
    bool full() const { return size() >= cap; }
};

// ============================================================================
// 2. RING BUFFER QUEUE (Lock-free, CAS-based)
// ============================================================================
template<typename T>
class RingBufferQueue {
private:
    static constexpr size_t CACHE_LINE = 64;
    
    struct Slot {
        alignas(CACHE_LINE) std::atomic<bool> occupied;
        T data;
        
        Slot() : occupied(false), data() {}
        Slot(const Slot&) = delete;
        Slot& operator=(const Slot&) = delete;
    };
    
    Slot* buffer;
    alignas(CACHE_LINE) std::atomic<size_t> write_pos{0};
    alignas(CACHE_LINE) std::atomic<size_t> read_pos{0};
    size_t cap;
    size_t mask;
    std::atomic<size_t> sz{0};
    
public:
    explicit RingBufferQueue(size_t capacity) : buffer(nullptr), cap(1), mask(0) {
        if (capacity == 0) throw std::invalid_argument("Capacity must be positive");
        
        while (cap < capacity) cap <<= 1;
        mask = cap - 1;
        
        buffer = static_cast<Slot*>(operator new[](sizeof(Slot) * cap));
        for (size_t i = 0; i < cap; ++i) {
            new (&buffer[i]) Slot();
        }
    }
    
    ~RingBufferQueue() {
        for (size_t i = 0; i < cap; ++i) {
            buffer[i].~Slot();
        }
        operator delete[](buffer);
    }
    
    RingBufferQueue(const RingBufferQueue&) = delete;
    RingBufferQueue& operator=(const RingBufferQueue&) = delete;
    
    bool enqueue(T item) {
        size_t w = write_pos.load(std::memory_order_relaxed);
        while (true) {
            size_t r = read_pos.load(std::memory_order_acquire);
            if (w - r >= cap) return false;
            if (write_pos.compare_exchange_weak(w, w + 1, std::memory_order_acq_rel)) {
                size_t idx = w & mask;
                while (buffer[idx].occupied.load(std::memory_order_acquire)) {
                    __builtin_ia32_pause();
                }
                buffer[idx].data = std::move(item);
                buffer[idx].occupied.store(true, std::memory_order_release);
                sz.fetch_add(1, std::memory_order_release);
                return true;
            }
        }
    }
    
    bool dequeue(T& item) {
        size_t r = read_pos.load(std::memory_order_relaxed);
        while (true) {
            size_t w = write_pos.load(std::memory_order_acquire);
            if (r >= w) return false;
            if (read_pos.compare_exchange_weak(r, r + 1, std::memory_order_acq_rel)) {
                size_t idx = r & mask;
                while (!buffer[idx].occupied.load(std::memory_order_acquire)) {
                    __builtin_ia32_pause();
                }
                item = std::move(buffer[idx].data);
                buffer[idx].occupied.store(false, std::memory_order_release);
                sz.fetch_sub(1, std::memory_order_release);
                return true;
            }
        }
    }
    
    size_t size() const { return sz.load(std::memory_order_acquire); }
    bool empty() const { return size() == 0; }
    bool full() const { return size() >= cap; }
};

// ============================================================================
// 3. BOUNDED MPMC QUEUE (Simplified, using CAS - works reliably)
// ============================================================================
template<typename T>
class BoundedMPMCQueue {
private:
    static constexpr size_t CACHE_LINE = 64;
    
    struct Slot {
        alignas(CACHE_LINE) std::atomic<bool> occupied;
        T data;
        
        Slot() : occupied(false), data() {}
        Slot(const Slot&) = delete;
        Slot& operator=(const Slot&) = delete;
    };
    
    Slot* buffer;
    alignas(CACHE_LINE) std::atomic<size_t> head{0};
    alignas(CACHE_LINE) std::atomic<size_t> tail{0};
    size_t cap;
    size_t mask;
    
public:
    explicit BoundedMPMCQueue(size_t capacity) : buffer(nullptr), cap(1), mask(0) {
        if (capacity == 0) throw std::invalid_argument("Capacity must be positive");
        
        while (cap < capacity) cap <<= 1;
        mask = cap - 1;
        
        buffer = static_cast<Slot*>(operator new[](sizeof(Slot) * cap));
        for (size_t i = 0; i < cap; ++i) {
            new (&buffer[i]) Slot();
        }
    }
    
    ~BoundedMPMCQueue() {
        for (size_t i = 0; i < cap; ++i) {
            buffer[i].~Slot();
        }
        operator delete[](buffer);
    }
    
    BoundedMPMCQueue(const BoundedMPMCQueue&) = delete;
    BoundedMPMCQueue& operator=(const BoundedMPMCQueue&) = delete;
    
    bool enqueue(T item) {
        while (true) {
            size_t t = tail.load(std::memory_order_acquire);
            size_t h = head.load(std::memory_order_acquire);
            
            if (t - h >= cap) {
                return false;  // Queue full
            }
            
            size_t idx = t & mask;
            
            // Check if slot is available
            if (buffer[idx].occupied.load(std::memory_order_acquire)) {
                // Slot should not be occupied if we're at tail
                // This means something went wrong, retry
                __builtin_ia32_pause();
                continue;
            }
            
            // Try to claim this slot
            if (tail.compare_exchange_weak(t, t + 1, 
                    std::memory_order_release, std::memory_order_relaxed)) {
                buffer[idx].data = std::move(item);
                buffer[idx].occupied.store(true, std::memory_order_release);
                return true;
            }
        }
    }
    
    bool dequeue(T& item) {
        while (true) {
            size_t h = head.load(std::memory_order_acquire);
            size_t t = tail.load(std::memory_order_acquire);
            
            if (h >= t) {
                return false;  // Queue empty
            }
            
            size_t idx = h & mask;
            
            // Check if slot has data
            if (!buffer[idx].occupied.load(std::memory_order_acquire)) {
                // Slot should have data if we're at head
                __builtin_ia32_pause();
                continue;
            }
            
            // Try to claim this slot
            if (head.compare_exchange_weak(h, h + 1,
                    std::memory_order_release, std::memory_order_relaxed)) {
                item = std::move(buffer[idx].data);
                buffer[idx].occupied.store(false, std::memory_order_release);
                return true;
            }
        }
    }
    
    size_t size() const {
        size_t t = tail.load(std::memory_order_acquire);
        size_t h = head.load(std::memory_order_acquire);
        return t - h;
    }
    
    bool empty() const { return size() == 0; }
    bool full() const { return size() >= cap; }
    size_t capacity() const { return cap; }
};

#endif
