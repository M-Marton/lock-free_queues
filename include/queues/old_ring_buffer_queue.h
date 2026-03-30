/**
 * @file ring_buffer_queue.h
 * @brief High-performance bounded MPMC ring buffer queue
 * @author MPMC Benchmark Project
 * @version 1.0.0
 * 
 * @ingroup queues
 * @{
 */

#pragma once

#include <atomic>
#include <vector>
#include <cstdint>
#include <thread>

/**
 * @brief Bounded MPMC queue using a ring buffer with atomic indices
 * 
 * This implementation uses a fixed-size circular buffer with separate
 * atomic indices for write and read positions. It achieves lock-free
 * behavior with minimal overhead and predictable memory usage.
 * 
 * @tparam T The type of elements stored in the queue. Must be movable.
 * 
 * @performance Enqueue and dequeue are wait-free under low contention
 *              and spin under high contention. Cache-friendly due to
 *              contiguous memory layout.
 * 
 * @complexity O(1) for both enqueue and dequeue operations
 * @memory Bounded with fixed capacity determined at construction
 * @thread_safety Fully lock-free and thread-safe for MPMC operations
 * 
 * @warning Capacity is rounded up to the next power of two for efficient
 *          modulo using bitwise AND. The actual capacity may be larger
 *          than requested.
 * 
 * @see MutexMPMCQueue for blocking baseline
 * @see TwoLockMPMCQueue for unbounded alternative
 */
template<typename T>
class BoundedRingBufferMPMC {
private:
    /**
     * @brief Cache line alignment for false sharing prevention
     */
    static constexpr size_t CACHE_LINE_SIZE = 64;
    
    /**
     * @brief Individual slot in the ring buffer
     * 
     * Each slot is aligned to cache line boundary to prevent false sharing
     * between adjacent slots.
     */
    struct Slot {
        alignas(CACHE_LINE_SIZE) std::atomic<bool> has_data{false};
        T data;
        
        Slot() = default;
        Slot(const Slot&) = delete;
        Slot& operator=(const Slot&) = delete;
    };
    
    std::vector<Slot> buffer_;              ///< Ring buffer storage
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> write_pos_{0};  ///< Next write position
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> read_pos_{0};    ///< Next read position
    size_t capacity_;                       ///< Buffer capacity (power of two)
    size_t mask_;                           ///< capacity_ - 1 for fast modulo
    
public:
    /**
     * @brief Construct a bounded ring buffer queue
     * @param capacity Maximum number of elements (rounded up to next power of two)
     * 
     * @pre capacity > 0
     * @post Actual capacity is the smallest power of two >= requested capacity
     */
    explicit BoundedRingBufferMPMC(size_t capacity) 
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
     * @brief Enqueue an element into the queue
     * @param item Element to enqueue (moved)
     * @return true if successfully enqueued, false if queue is full
     * 
     * @thread_safety Safe for concurrent calls by multiple producers
     * @lock_free Uses atomic compare-and-swap without blocking
     * 
     * @performance May spin if the write position is contended.
     *              Memory ordering: acquire-release for proper synchronization.
     * 
     * @algorithm
     * 1. Atomically increment write position
     * 2. Compute slot index using mask
     * 3. Spin until slot is free
     * 4. Write data and mark slot as ready
     */
    bool enqueue(T item) {
        size_t write = write_pos_.load(std::memory_order_relaxed);
        
        while (true) {
            size_t read = read_pos_.load(std::memory_order_acquire);
            
            // Check if queue is full
            if (write - read >= capacity_) {
                return false;
            }
            
            // Try to claim the write position
            if (write_pos_.compare_exchange_weak(write, write + 1,
                    std::memory_order_acq_rel)) {
                
                size_t idx = write & mask_;
                
                // Wait for slot to be ready (should be immediate)
                while (buffer_[idx].has_data.load(std::memory_order_acquire)) {
                    __builtin_ia32_pause();  // Pause instruction for spin-wait
                }
                
                // Write data
                buffer_[idx].data = std::move(item);
                buffer_[idx].has_data.store(true, std::memory_order_release);
                return true;
            }
        }
    }
    
    /**
     * @brief Dequeue an element from the queue
     * @param item Output parameter for the dequeued element
     * @return true if successfully dequeued, false if queue is empty
     * 
     * @thread_safety Safe for concurrent calls by multiple consumers
     * @lock_free Uses atomic compare-and-swap without blocking
     * 
     * @performance May spin if the read position is contended.
     *              Memory ordering: acquire-release for proper synchronization.
     * 
     * @algorithm
     * 1. Atomically increment read position
     * 2. Compute slot index using mask
     * 3. Spin until slot has data
     * 4. Read data and mark slot as free
     */
    bool dequeue(T& item) {
        size_t read = read_pos_.load(std::memory_order_relaxed);
        
        while (true) {
            size_t write = write_pos_.load(std::memory_order_acquire);
            
            // Check if queue is empty
            if (read >= write) {
                return false;
            }
            
            // Try to claim the read position
            if (read_pos_.compare_exchange_weak(read, read + 1,
                    std::memory_order_acq_rel)) {
                
                size_t idx = read & mask_;
                
                // Wait for data to be ready
                while (!buffer_[idx].has_data.load(std::memory_order_acquire)) {
                    __builtin_ia32_pause();  // Pause instruction for spin-wait
                }
                
                // Read data
                item = std::move(buffer_[idx].data);
                buffer_[idx].has_data.store(false, std::memory_order_release);
                return true;
            }
        }
    }
    
    /**
     * @brief Get the current number of elements in the queue
     * @return Number of elements (approximate, not exact in concurrent scenarios)
     * 
     * @performance Very fast, only uses relaxed atomic loads
     * @note The returned value may be stale due to concurrent operations
     */
    size_t size() const {
        size_t write = write_pos_.load(std::memory_order_relaxed);
        size_t read = read_pos_.load(std::memory_order_relaxed);
        return write - read;
    }
    
    /**
     * @brief Check if the queue is empty
     * @return true if empty, false otherwise
     */
    bool empty() const { return size() == 0; }
    
    /**
     * @brief Check if the queue is full
     * @return true if full, false otherwise
     */
    bool full() const { return size() >= capacity_; }
    
    /**
     * @brief Get the actual capacity of the queue
     * @return Maximum number of elements the queue can hold
     */
    size_t capacity() const { return capacity_; }
};

/** @} */
