#ifndef TWO_LOCK_QUEUE_H
#define TWO_LOCK_QUEUE_H

#include <atomic>
#include <memory>
#include <cstdint>

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
    
public:
    TwoLockMPMCQueue() {
        Node* dummy = new Node();
        head_.store(dummy, std::memory_order_relaxed);
        tail_.store(dummy, std::memory_order_relaxed);
    }
    
    ~TwoLockMPMCQueue() {
        T item;
        while (dequeue(item)) {}
        
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
            Node* tail = tail_.load(std::memory_order_acquire);
            Node* next = tail->next.load(std::memory_order_acquire);
            
            if (tail != tail_.load(std::memory_order_acquire)) {
                continue;
            }
            
            if (next != nullptr) {
                tail_.compare_exchange_weak(tail, next,
                    std::memory_order_release,
                    std::memory_order_relaxed);
                continue;
            }
            
            if (tail->next.compare_exchange_weak(next, new_node,
                    std::memory_order_release,
                    std::memory_order_relaxed)) {
                tail_.compare_exchange_weak(tail, new_node,
                    std::memory_order_release,
                    std::memory_order_relaxed);
                size_.fetch_add(1, std::memory_order_release);
                return true;
            }
        }
    }
    
    bool dequeue(T& item) {
        while (true) {
            Node* head = head_.load(std::memory_order_acquire);
            Node* tail = tail_.load(std::memory_order_acquire);
            Node* next = head->next.load(std::memory_order_acquire);
            
            if (head != head_.load(std::memory_order_acquire)) {
                continue;
            }
            
            if (head == tail) {
                if (next == nullptr) {
                    return false;
                }
                tail_.compare_exchange_weak(tail, next,
                    std::memory_order_release,
                    std::memory_order_relaxed);
                continue;
            }
            
            if (next && next->data) {
                item = std::move(*next->data);
            }
            
            if (head_.compare_exchange_weak(head, next,
                    std::memory_order_release,
                    std::memory_order_relaxed)) {
                delete head;
                size_.fetch_sub(1, std::memory_order_release);
                return true;
            }
        }
    }
    
    size_t size() const {
        return size_.load(std::memory_order_acquire);
    }
    
    bool empty() const { return size() == 0; }
    
    TwoLockMPMCQueue(const TwoLockMPMCQueue&) = delete;
    TwoLockMPMCQueue& operator=(const TwoLockMPMCQueue&) = delete;
};

#endif
