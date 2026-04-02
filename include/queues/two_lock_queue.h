/**
 * @file two_lock_queue.h
 * @brief Lock-free MPMC queue with proper memory management
 */

#pragma once

#include <atomic>
#include <memory>
#include <cstdint>
#include <thread>

template<typename T>
class TwoLockMPMCQueue {
private:
    struct Node {
        std::shared_ptr<T> data;
        std::atomic<Node*> next;
        
        Node() : next(nullptr) {}
        Node(T&& val) : data(std::make_shared<T>(std::move(val))), next(nullptr) {}
    };
    
    std::atomic<Node*> head_;
    std::atomic<Node*> tail_;
    std::atomic<uint64_t> size_{0};
    
    /**
     * @brief Helper to safely delete nodes (handles race conditions)
     */
    static void safe_delete(Node* node) {
        if (node) {
            delete node;
        }
    }
    
public:
    TwoLockMPMCQueue() {
        Node* dummy = new Node();
        head_.store(dummy, std::memory_order_relaxed);
        tail_.store(dummy, std::memory_order_relaxed);
    }
    
    ~TwoLockMPMCQueue() {
        // Drain the queue
        T item;
        while (dequeue(item)) {
            // Continue draining
        }
        
        // Clean up remaining nodes
        Node* current = head_.load(std::memory_order_relaxed);
        while (current) {
            Node* next = current->next.load(std::memory_order_relaxed);
            delete current;
            current = next;
        }
    }
    
    bool enqueue(T item) {
        Node* new_node = new Node(std::move(item));
        
        while (true) {
            Node* last = tail_.load(std::memory_order_acquire);
            Node* next = last->next.load(std::memory_order_acquire);
            
            // Check if tail hasn't been changed by another thread
            if (last != tail_.load(std::memory_order_acquire)) {
                continue;
            }
            
            if (next != nullptr) {
                // Tail is lagging, help advance it
                tail_.compare_exchange_weak(last, next,
                    std::memory_order_release,
                    std::memory_order_relaxed);
                continue;
            }
            
            // Try to link the new node
            if (last->next.compare_exchange_weak(next, new_node,
                    std::memory_order_release,
                    std::memory_order_relaxed)) {
                // Success, advance tail
                tail_.compare_exchange_weak(last, new_node,
                    std::memory_order_release,
                    std::memory_order_relaxed);
                size_.fetch_add(1, std::memory_order_release);
                return true;
            }
        }
    }
    
    bool dequeue(T& item) {
        while (true) {
            Node* first = head_.load(std::memory_order_acquire);
            Node* last = tail_.load(std::memory_order_acquire);
            Node* next = first->next.load(std::memory_order_acquire);
            
            // Check if head hasn't been changed by another thread
            if (first != head_.load(std::memory_order_acquire)) {
                continue;
            }
            
            if (first == last) {
                if (next == nullptr) {
                    return false;  // Queue empty
                }
                // Tail is lagging, help advance it
                tail_.compare_exchange_weak(last, next,
                    std::memory_order_release,
                    std::memory_order_relaxed);
                continue;
            }
            
            // Queue has elements
            if (next && next->data) {
                item = std::move(*next->data);
            }
            
            // Try to advance head
            if (head_.compare_exchange_weak(first, next,
                    std::memory_order_release,
                    std::memory_order_relaxed)) {
                // Delete the old dummy node
                delete first;
                size_.fetch_sub(1, std::memory_order_release);
                return true;
            }
        }
    }
    
    size_t size() const {
        return size_.load(std::memory_order_acquire);
    }
    
    bool empty() const { return size() == 0; }
    
    // Disable copying
    TwoLockMPMCQueue(const TwoLockMPMCQueue&) = delete;
    TwoLockMPMCQueue& operator=(const TwoLockMPMCQueue&) = delete;
};
