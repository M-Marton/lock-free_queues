#ifndef TWO_LOCK_QUEUE_HAZARD_H
#define TWO_LOCK_QUEUE_HAZARD_H

#include <atomic>
#include <memory>
#include <vector>
#include <thread>
#include <cstdint>
#include <algorithm>
#include <mutex>
#include <functional>
#include <unordered_map>

// Forward declaration
template<typename T>
class TwoLockMPMCQueueHazard;

// Hazard pointer manager that works with any node type using type-erasure
class HazardPointerManager {
private:
    static constexpr size_t MAX_THREADS = 256;
    static constexpr size_t RETIRE_DELAY = 100;
    
    struct HazardPointer {
        std::atomic<void*> pointer{nullptr};
        std::atomic<bool> active{false};
    };
    
    struct RetiredNode {
        void* node;
        std::function<void(void*)> deleter;
        uint64_t epoch;
    };
    
    std::vector<HazardPointer> hazard_pointers_;
    std::vector<RetiredNode> retired_nodes_;
    std::mutex retire_mutex_;
    std::atomic<uint64_t> global_epoch_{0};
    
public:
    HazardPointerManager() : hazard_pointers_(MAX_THREADS) {}
    
    void* protect(size_t thread_id, std::atomic<void*>& source) {
        void* ptr = source.load(std::memory_order_acquire);
        HazardPointer& hp = hazard_pointers_[thread_id % MAX_THREADS];
        hp.pointer.store(ptr, std::memory_order_relaxed);
        hp.active.store(true, std::memory_order_release);
        
        if (source.load(std::memory_order_acquire) != ptr) {
            hp.pointer.store(nullptr, std::memory_order_relaxed);
            return protect(thread_id, source);
        }
        
        return ptr;
    }
    
    void release(size_t thread_id) {
        HazardPointer& hp = hazard_pointers_[thread_id % MAX_THREADS];
        hp.active.store(false, std::memory_order_release);
        hp.pointer.store(nullptr, std::memory_order_relaxed);
    }
    
    template<typename NodeType>
    void retire(NodeType* node) {
        std::lock_guard<std::mutex> lock(retire_mutex_);
        retired_nodes_.push_back({
            node, 
            [](void* n) { delete static_cast<NodeType*>(n); },
            global_epoch_.load()
        });
        
        if (retired_nodes_.size() >= RETIRE_DELAY) {
            scan();
        }
    }
    
    void scan() {
        std::vector<void*> active_pointers;
        
        for (auto& hp : hazard_pointers_) {
            if (hp.active.load(std::memory_order_acquire)) {
                void* ptr = hp.pointer.load(std::memory_order_relaxed);
                if (ptr) {
                    active_pointers.push_back(ptr);
                }
            }
        }
        
        std::sort(active_pointers.begin(), active_pointers.end());
        
        auto it = retired_nodes_.begin();
        while (it != retired_nodes_.end()) {
            if (!std::binary_search(active_pointers.begin(), 
                                    active_pointers.end(), 
                                    it->node)) {
                it->deleter(it->node);
                it = retired_nodes_.erase(it);
            } else {
                ++it;
            }
        }
        
        global_epoch_.fetch_add(1, std::memory_order_relaxed);
    }
    
    static HazardPointerManager& instance() {
        static HazardPointerManager manager;
        return manager;
    }
};

template<typename T>
class TwoLockMPMCQueueHazard {
public:
    struct Node {
        std::shared_ptr<T> data;
        std::atomic<Node*> next;
        
        Node() : next(nullptr) {}
        Node(T&& val) : data(std::make_shared<T>(std::move(val))), next(nullptr) {}
    };
    
private:
    std::atomic<Node*> head_;
    std::atomic<Node*> tail_;
    std::atomic<uint64_t> size_{0};
    
    static size_t get_thread_id() {
        static std::atomic<size_t> next_id{0};
        static thread_local size_t thread_id = next_id.fetch_add(1);
        return thread_id;
    }
    
public:
    TwoLockMPMCQueueHazard() {
        Node* dummy = new Node();
        head_.store(dummy, std::memory_order_relaxed);
        tail_.store(dummy, std::memory_order_relaxed);
    }
    
    ~TwoLockMPMCQueueHazard() {
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
            if (!tail) {
                delete new_node;
                return false;
            }
            
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
        size_t thread_id = get_thread_id();
        
        while (true) {
            Node* head = head_.load(std::memory_order_acquire);
            if (!head) return false;
            
            Node* tail = tail_.load(std::memory_order_acquire);
            
            // Protect head with hazard pointer
            HazardPointerManager::instance().protect(thread_id, 
                reinterpret_cast<std::atomic<void*>&>(head_));
            
            // Re-check head after protection
            if (head != head_.load(std::memory_order_acquire)) {
                HazardPointerManager::instance().release(thread_id);
                continue;
            }
            
            Node* next = head->next.load(std::memory_order_acquire);
            
            if (head == tail) {
                HazardPointerManager::instance().release(thread_id);
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
                HazardPointerManager::instance().release(thread_id);
                // Safely retire the old node using the templated retire method
                HazardPointerManager::instance().retire<Node>(head);
                size_.fetch_sub(1, std::memory_order_release);
                return true;
            }
            
            HazardPointerManager::instance().release(thread_id);
        }
    }
    
    size_t size() const {
        return size_.load(std::memory_order_acquire);
    }
    
    bool empty() const {
        return size() == 0;
    }
    
    // Disable copying
    TwoLockMPMCQueueHazard(const TwoLockMPMCQueueHazard&) = delete;
    TwoLockMPMCQueueHazard& operator=(const TwoLockMPMCQueueHazard&) = delete;
};

#endif
