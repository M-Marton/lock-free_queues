#ifndef MUTEX_QUEUE_H
#define MUTEX_QUEUE_H

#include <queue>
#include <mutex>
#include <atomic>
#include <cstdint>
#include <stdexcept>

template<typename T>
class MutexMPMCQueue {
private:
    std::queue<T> queue_;
    mutable std::mutex mutex_;
    size_t capacity_;
    std::atomic<uint64_t> size_{0};
    
public:
    explicit MutexMPMCQueue(size_t capacity) : capacity_(capacity) {
        if (capacity == 0) {
            throw std::invalid_argument("Capacity must be positive");
        }
    }
    
    bool enqueue(T item) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() >= capacity_) {
            return false;
        }
        queue_.push(std::move(item));
        size_.fetch_add(1, std::memory_order_release);
        return true;
    }
    
    bool dequeue(T& item) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            return false;
        }
        item = std::move(queue_.front());
        queue_.pop();
        size_.fetch_sub(1, std::memory_order_release);
        return true;
    }
    
    size_t size() const {
        return size_.load(std::memory_order_acquire);
    }
    
    bool empty() const { return size() == 0; }
    bool full() const { return size() >= capacity_; }
    size_t capacity() const { return capacity_; }
};

#endif
