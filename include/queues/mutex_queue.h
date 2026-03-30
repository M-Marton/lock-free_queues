/**
 * @file mutex_queue.h
 * @brief Baseline MPMC queue implementation using std::mutex and condition variables
 * @author MPMC Benchmark Project
 * @version 1.0.0
 * 
 * @defgroup queues Queue Implementations
 * @brief MPMC queue implementations for benchmarking
 * 
 * @ingroup queues
 * @{
 */

#pragma once

#include <queue>
#include <mutex>
#include <condition_variable>
#include <stdexcept>
#include <atomic>
#include <cstdint>
#include <chrono>

/**
 * @brief Baseline MPMC queue using mutex-based synchronization
 * 
 * This is a simple bounded queue implementation using std::mutex and
 * condition variables. It serves as a baseline for comparing lock-free
 * implementations.
 * 
 * @tparam T The type of elements stored in the queue
 * 
 * @performance This implementation blocks threads when the queue is full
 *              or empty, which can lead to high context switch overhead
 *              under contention.
 * 
 * @complexity Enqueue and dequeue are O(1) amortized
 * @thread_safety Fully thread-safe for MPMC operations
 * 
 * @warning This implementation is not lock-free; threads may block
 * 
 * @see TwoLockMPMCQueue for lock-free alternative
 * @see BoundedRingBufferMPMC for bounded lock-free alternative
 */
template<typename T>
class MutexMPMCQueue {
private:
    std::queue<T> queue_;               ///< Underlying queue storage
    mutable std::mutex mutex_;          ///< Mutex for protecting queue_ access
    std::condition_variable not_empty_; ///< Signaled when queue becomes non-empty
    std::condition_variable not_full_;  ///< Signaled when queue becomes non-full
    size_t capacity_;                   ///< Maximum queue size
    std::atomic<uint64_t> size_{0};     ///< Current size (atomic for fast access)
    
public:
    /**
     * @brief Construct a bounded mutex queue
     * @param capacity Maximum number of elements the queue can hold
     * 
     * @pre capacity > 0
     */
    explicit MutexMPMCQueue(size_t capacity) : capacity_(capacity) {
        if (capacity == 0) {
            throw std::invalid_argument("Capacity must be positive");
        }
    }
    
    /**
     * @brief Enqueue an element into the queue
     * @param item Element to enqueue (moved)
     * @return true if enqueued successfully, false if queue is full
     * 
     * @thread_safety Safe for concurrent calls by multiple producers
     * @blocks If the queue is full, waits until space becomes available
     * 
     * @performance Under contention, this function may block and cause
     *              context switches. For high-throughput scenarios, consider
     *              lock-free alternatives.
     */
    bool enqueue(T item) {
        /*
	############## POSSIBLE DEADLOCK ############
	std::unique_lock<std::mutex> lock(mutex_);
	not_full_.wait(lock, [this] { return queue_.size() < capacity_; });
	*/    

	std::lock_guard<std::mutex> lock(mutex_);
        
        if (queue_.size() >= capacity_) {
            return false;  // Queue full, return immediately
        }	

        queue_.push(std::move(item));
        size_.fetch_add(1, std::memory_order_release);
        //not_empty_.notify_one();
        return true;
    }
    
    /**
     * @brief Dequeue an element from the queue
     * @param item Output parameter for the dequeued element
     * @return true if dequeued successfully, false if queue is empty
     * 
     * @thread_safety Safe for concurrent calls by multiple consumers
     * @blocks If the queue is empty, waits until an element becomes available
     * 
     * @performance Under contention, this function may block and cause
     *              context switches.
     */
    bool dequeue(T& item) {
        

	/*
	############ POSSIBLE DEADLOCK##########
	std::unique_lock<std::mutex> lock(mutex_);
	not_empty_.wait(lock, [this] { return !queue_.empty(); });
        */
	std::lock_guard<std::mutex> lock(mutex_);
        
        if (queue_.empty()) {
            return false;  // Queue empty, return immediately
        }	

        item = std::move(queue_.front());
        queue_.pop();
        size_.fetch_sub(1, std::memory_order_release);
        //not_full_.notify_one();

	
	return true;
    }
    
    /**
     * @brief Get the current size of the queue
     * @return Number of elements currently in the queue (approximate)
     * 
     * @note This function is atomic but may return a slightly stale value
     *       due to concurrent updates.
     */
    size_t size() const {
        return size_.load(std::memory_order_acquire);
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
     * @brief Get the capacity of the queue
     * @return Maximum number of elements the queue can hold
     */
    size_t capacity() const { return capacity_; }
};

/** @} */
