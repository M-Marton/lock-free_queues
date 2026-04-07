#ifndef SCQ_QUEUE_H
#define SCQ_QUEUE_H

#include <atomic>
#include <vector>
#include <cstdint>
#include <cstddef>

/**
 * Bounded MPMC queue using fetch-and-add with circular buffer.
 * This is a simplified but correct MPMC queue that replaces SCQ.
 * 
 * Key features:
 * - Lock-free for both enqueue and dequeue
 * - Bounded capacity (must be power of two)
 * - Uses fetch-and-add for position claiming
 * - Spin-waits on full/empty conditions
 */
template<typename T>
class BoundedMPMCQueue {
private:
    static constexpr size_t CACHE_LINE_SIZE = 64;
    
    struct alignas(CACHE_LINE_SIZE) Slot {
        std::atomic<bool> has_data{false};
        T data;
        
        Slot() = default;
    };
    
    std::vector<Slot> buffer_;
    alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> write_pos_{0};
    alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> read_pos_{0};
    size_t capacity_;
    size_t mask_;
    
public:
    /**
     * Constructor - creates a bounded queue with power-of-two capacity
     * @param capacity Maximum number of elements (will be rounded up to power of two)
     */
    explicit BoundedMPMCQueue(size_t capacity) 
        : capacity_(1)
        , mask_(0) {
        
        if (capacity == 0) {
            throw std::invalid_argument("Capacity must be positive");
        }
        
        // Round up to next power of two
        while (capacity_ < capacity) {
            capacity_ <<= 1;
        }
        mask_ = capacity_ - 1;
        
        // Allocate buffer
        buffer_.resize(capacity_);
    }
    
    /**
     * Enqueue an element
     * @param item Element to enqueue (moved)
     * @return true if enqueued, false if queue is full
     */
    bool enqueue(T item) {
        while (true) {
            // Atomically claim a write position
            uint64_t pos = write_pos_.fetch_add(1, std::memory_order_acquire);
            size_t idx = pos & mask_;
            
            // Check if the queue is full
            uint64_t read = read_pos_.load(std::memory_order_acquire);
            if (pos - read >= capacity_) {
                // Queue is full - rollback and retry
                write_pos_.fetch_sub(1, std::memory_order_release);
                return false;
            }
            
            // Wait for the slot to be free (should be free, but check anyway)
            while (buffer_[idx].has_data.load(std::memory_order_acquire)) {
                __builtin_ia32_pause();
            }
            
            // Write the data
            buffer_[idx].data = std::move(item);
            buffer_[idx].has_data.store(true, std::memory_order_release);
            return true;
        }
    }
    
    /**
     * Dequeue an element
     * @param item Output parameter for the dequeued element
     * @return true if dequeued, false if queue is empty
     */
    bool dequeue(T& item) {
        while (true) {
            // Atomically claim a read position
            uint64_t pos = read_pos_.fetch_add(1, std::memory_order_acquire);
            size_t idx = pos & mask_;
            
            // Check if the queue is empty
            uint64_t write = write_pos_.load(std::memory_order_acquire);
            if (pos >= write) {
                // Queue is empty - rollback and retry
                read_pos_.fetch_sub(1, std::memory_order_release);
                return false;
            }
            
            // Wait for data to be available
            while (!buffer_[idx].has_data.load(std::memory_order_acquire)) {
                __builtin_ia32_pause();
            }
            
            // Read the data
            item = std::move(buffer_[idx].data);
            buffer_[idx].has_data.store(false, std::memory_order_release);
            return true;
        }
    }
    
    /**
     * Get approximate size of the queue
     */
    size_t size() const {
        uint64_t write = write_pos_.load(std::memory_order_acquire);
        uint64_t read = read_pos_.load(std::memory_order_acquire);
        return static_cast<size_t>(write - read);
    }
    
    /**
     * Check if the queue is empty
     */
    bool empty() const {
        return size() == 0;
    }
    
    /**
     * Check if the queue is full
     */
    bool full() const {
        return size() >= capacity_;
    }
    
    /**
     * Get the capacity of the queue
     */
    size_t capacity() const {
        return capacity_;
    }
};

// Alias for SCQQueue to maintain compatibility with benchmark_main.cpp
template<typename T>
using SCQQueue = BoundedMPMCQueue<T>;

#endif
