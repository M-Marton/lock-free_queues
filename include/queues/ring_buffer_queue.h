#ifndef RING_BUFFER_QUEUE_H
#define RING_BUFFER_QUEUE_H

#include <atomic>
#include <memory>
#include <cstdint>
#include <stdexcept>

template<typename T>
class BoundedRingBufferMPMC {
private:
    static constexpr size_t CACHE_LINE_SIZE = 64;
    
    struct Slot {
        std::atomic<bool> has_data;
        T data;
        
        Slot() : has_data(false), data() {}
    };
    
    std::unique_ptr<Slot[]> buffer_;
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> write_pos_{0};
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> read_pos_{0};
    size_t capacity_;
    size_t mask_;
    std::atomic<uint64_t> size_{0};
    
public:
    explicit BoundedRingBufferMPMC(size_t capacity) 
        : capacity_(1)
        , mask_(0) {
        
        if (capacity == 0) {
            throw std::invalid_argument("Capacity must be positive");
        }
        
        while (capacity_ < capacity) {
            capacity_ <<= 1;
        }
        mask_ = capacity_ - 1;
        
        buffer_ = std::make_unique<Slot[]>(capacity_);
    }
    
    bool enqueue(T item) {
        size_t write = write_pos_.load(std::memory_order_relaxed);
        
        while (true) {
            size_t read = read_pos_.load(std::memory_order_acquire);
            
            if (write - read >= capacity_) {
                return false;
            }
            
            if (write_pos_.compare_exchange_weak(write, write + 1,
                    std::memory_order_acq_rel)) {
                
                size_t idx = write & mask_;
                Slot& slot = buffer_[idx];
                
                while (slot.has_data.load(std::memory_order_acquire)) {
                    __builtin_ia32_pause();
                }
                
                slot.data = std::move(item);
                slot.has_data.store(true, std::memory_order_release);
                size_.fetch_add(1, std::memory_order_release);
                return true;
            }
        }
    }
    
    bool dequeue(T& item) {
        size_t read = read_pos_.load(std::memory_order_relaxed);
        
        while (true) {
            size_t write = write_pos_.load(std::memory_order_acquire);
            
            if (read >= write) {
                return false;
            }
            
            if (read_pos_.compare_exchange_weak(read, read + 1,
                    std::memory_order_acq_rel)) {
                
                size_t idx = read & mask_;
                Slot& slot = buffer_[idx];
                
                while (!slot.has_data.load(std::memory_order_acquire)) {
                    __builtin_ia32_pause();
                }
                
                item = std::move(slot.data);
                slot.has_data.store(false, std::memory_order_release);
                size_.fetch_sub(1, std::memory_order_release);
                return true;
            }
        }
    }
    
    size_t size() const {
        return size_.load(std::memory_order_acquire);
    }
    
    bool empty() const { return size() == 0; }
    bool full() const { return size() >= capacity_; }
    size_t capacity() const { return capacity_; }
};

#endif
