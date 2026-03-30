/**
 * @file two_lock_queue.h
 * @brief Lock-free MPMC queue implementation using Michael-Scott algorithm
 * @author MPMC Benchmark Project
 * @version 1.0.0
 * 
 * @ingroup queues
 * @{
 */

#pragma once

#include <atomic>
#include <memory>
#include <cstdint>
#include <thread>

/**
 * @brief Lock-free MPMC queue using Michael-Scott algorithm
 * 
 * This implementation uses a linked list structure with atomic compare-and-swap
 * operations to achieve lock-free behavior for both enqueue and dequeue operations.
 * It is unbounded and can grow dynamically as elements are added.
 * 
 * @tparam T The type of elements stored in the queue
 * 
 * @algorithm Based on "Simple, Fast, and Practical Non-Blocking and Blocking
 *            Concurrent Queue Algorithms" by Michael and Scott (1996)
 * 
 * @performance Lock-free operations that do not block threads but may retry
 *              under contention. Provides good scalability across many cores.
 * 
 * @complexity Enqueue and dequeue are O(1) with CAS retries
 * @thread_safety Fully lock-free and thread-safe for MPMC operations
 * 
 * @warning The ABA problem is handled using pointer tagging or can be
 *          managed by the memory reclamation strategy.
 * 
 * @see MutexMPMCQueue for blocking baseline
 * @see BoundedRingBufferMPMC for bounded alternative
 */
template<typename T>
class TwoLockMPMCQueue {
private:
    /**
     * @brief Node in the linked list queue
     */
    struct Node {
        std::shared_ptr<T> data;        ///< Stored data (shared_ptr for safe memory management)
        std::atomic<Node*> next;        ///< Next node pointer
        
        /**
         * @brief Construct an empty node (dummy/header node)
         */
        Node() : next(nullptr) {}
        
        /**
         * @brief Construct a node with data
         * @param val Value to store (moved)
         */
        Node(T&& val) : data(std::make_shared<T>(std::move(val))), next(nullptr) {}
    };
    
    std::atomic<Node*> head_;           ///< Head pointer (consumer side)
    std::atomic<Node*> tail_;           ///< Tail pointer (producer side)
    
public:
    /**
     * @brief Construct an empty lock-free queue
     * 
     * Creates a dummy node that separates head and tail to simplify
     * concurrent operations.
     */
    TwoLockMPMCQueue() {
        Node* dummy = new Node();
        head_.store(dummy);
        tail_.store(dummy);
    }
    
    /**
     * @brief Destructor - cleans up remaining nodes
     */
    ~TwoLockMPMCQueue() {
        T item;
        while (dequeue(item)) {}
        delete head_.load();
    }
    
    /**
     * @brief Enqueue an element into the queue
     * @param item Element to enqueue (moved)
     * @return Always returns true (unbounded queue)
     * 
     * @thread_safety Safe for concurrent calls by multiple producers
     * @lock_free Uses compare-and-swap operations without blocking
     * 
     * @performance May retry if another producer is also enqueuing.
     *              Retries are bounded by the number of concurrent producers.
     * 
     * @algorithm
     * 1. Create new node with data
     * 2. Find the current tail
     * 3. Attempt to link new node after tail
     * 4. If successful, advance tail to new node
     * 5. If tail moved by another thread, retry from step 2
     */
    bool enqueue(T item) {
        Node* new_node = new Node(std::move(item));
        
        while (true) {
            Node* last = tail_.load(std::memory_order_acquire);
            Node* next = last->next.load(std::memory_order_acquire);
            
            // Check if tail hasn't been changed by another thread
            if (last == tail_.load(std::memory_order_acquire)) {
                if (next == nullptr) {
                    // Last node is the actual tail
                    if (last->next.compare_exchange_weak(next, new_node,
                            std::memory_order_release,
                            std::memory_order_relaxed)) {
                        // Successfully linked new node
                        tail_.compare_exchange_weak(last, new_node,
                            std::memory_order_release,
                            std::memory_order_relaxed);
                        return true;
                    }
                } else {
                    // Tail is lagging behind, help advance it
                    tail_.compare_exchange_weak(last, next,
                        std::memory_order_release,
                        std::memory_order_relaxed);
                }
            }
        }
    }
    
    /**
     * @brief Dequeue an element from the queue
     * @param item Output parameter for the dequeued element
     * @return true if element dequeued, false if queue empty
     * 
     * @thread_safety Safe for concurrent calls by multiple consumers
     * @lock_free Uses compare-and-swap operations without blocking
     * 
     * @performance May retry if another consumer is also dequeuing.
     *              Retries are bounded by the number of concurrent consumers.
     * 
     * @algorithm
     * 1. Read head and tail
     * 2. Check if queue is empty (head == tail and next is null)
     * 3. Attempt to advance head to next node
     * 4. If successful, return data from old head
     * 5. If head moved by another thread, retry from step 1
     */
    bool dequeue(T& item) {
        while (true) {
            Node* first = head_.load(std::memory_order_acquire);
            Node* last = tail_.load(std::memory_order_acquire);
            Node* next = first->next.load(std::memory_order_acquire);
            
            // Check if head hasn't been changed by another thread
            if (first == head_.load(std::memory_order_acquire)) {
                if (first == last) {
                    if (next == nullptr) {
                        // Queue is empty
                        return false;
                    }
                    // Tail is lagging, help advance it
                    tail_.compare_exchange_weak(last, next,
                        std::memory_order_release,
                        std::memory_order_relaxed);
                } else {
                    // Queue has elements
                    if (next->data) {
                        item = std::move(*next->data);
                    }
                    if (head_.compare_exchange_weak(first, next,
                            std::memory_order_release,
                            std::memory_order_relaxed)) {
                        delete first;  // Delete old dummy node
                        return true;
                    }
                }
            }
        }
    }
    
    /**
     * @brief Get the approximate size of the queue
     * @return Number of elements (approximate, not exact)
     * 
     * @note This function traverses the list and is O(n). It should only
     *       be used for debugging, not in production code.
     * @warning The result may be stale due to concurrent modifications
     */
    size_t size() const {
        size_t count = 0;
        Node* current = head_.load(std::memory_order_relaxed);
        while (current) {
            if (current->data) {
                count++;
            }
            current = current->next.load(std::memory_order_relaxed);
        }
        return count;
    }
    
    /**
     * @brief Check if the queue is empty
     * @return true if empty, false otherwise
     */
    bool empty() const {
        Node* first = head_.load(std::memory_order_acquire);
        Node* next = first->next.load(std::memory_order_acquire);
        return next == nullptr;
    }
    
    /**
     * @brief Disable copying
     */
    TwoLockMPMCQueue(const TwoLockMPMCQueue&) = delete;
    
    /**
     * @brief Disable assignment
     */
    TwoLockMPMCQueue& operator=(const TwoLockMPMCQueue&) = delete;
};

/** @} */
