#ifndef DISRUPTOR_QUEUE_H
#define DISRUPTOR_QUEUE_H

#include <atomic>
#include <vector>
#include <cstdint>
#include <thread>
#include <stdexcept>

template<typename T>
class DisruptorQueue {
private:
    static constexpr size_t CACHE_LINE_SIZE = 64;
    
    struct Slot {
        alignas(CACHE_LINE_SIZE) std::atomic<bool> published;
        T data;
        
        Slot() : published(false), data() {}
    };
    
    std::vector<Slot> buffer_;
    alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> cursor_{0};
    alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> read_index_{0};
    size_t capacity_;
    size_t mask_;
    
public:
    explicit DisruptorQueue(size_t capacity) 
        : buffer_(capacity)
        , capacity_(capacity)
        , mask_(capacity - 1) {
        
        if ((capacity & (capacity - 1)) != 0) {
            throw std::invalid_argument("Capacity must be power of two");
        }
    }
    
    bool enqueue(T item) {
        uint64_t pos = cursor_.fetch_add(1, std::memory_order_acquire);
        size_t idx = pos & mask_;
        
        while (buffer_[idx].published.load(std::memory_order_acquire)) {
            __builtin_ia32_pause();
        }
        
        buffer_[idx].data = std::move(item);
        buffer_[idx].published.store(true, std::memory_order_release);
        return true;
    }
    
    bool dequeue(T& item) {
        uint64_t pos = read_index_.load(std::memory_order_acquire);
        size_t idx = pos & mask_;
        
        if (!buffer_[idx].published.load(std::memory_order_acquire)) {
            return false;
        }
        
        item = std::move(buffer_[idx].data);
        buffer_[idx].published.store(false, std::memory_order_release);
        read_index_.store(pos + 1, std::memory_order_release);
        return true;
    }
    
    size_t size() const {
        uint64_t cursor = cursor_.load(std::memory_order_acquire);
        uint64_t read = read_index_.load(std::memory_order_acquire);
        return static_cast<size_t>(cursor - read);
    }
    
    bool empty() const { return size() == 0; }
    bool full() const { return size() >= capacity_; }
    size_t capacity() const { return capacity_; }
};

#endif
