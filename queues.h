#ifndef QUEUES_H
#define QUEUES_H

#include <atomic>
#include <queue>
#include <mutex>
#include <vector>
#include <cstdint>
#include <functional>
#include <algorithm>
#include <thread>
#include <memory>
#include <stdexcept>


// ============================================================================
// 1. MUTEX QUEUE (Baseline, blocking, bounded)
// ============================================================================
template<typename T>
class MutexQueue {
    std::queue<T> q;
    mutable std::mutex mtx;
    size_t cap;
    std::atomic<size_t> sz{0};
public:
    explicit MutexQueue(size_t capacity) : cap(capacity) {
        if (!capacity) throw std::invalid_argument("capacity>0");
    }
    bool enqueue(T item) {
        std::lock_guard<std::mutex> lock(mtx);
        if (q.size() >= cap) return false;
        q.push(std::move(item));
        sz.store(q.size(), std::memory_order_release);
        return true;
    }
    bool dequeue(T& item) {
        std::lock_guard<std::mutex> lock(mtx);
        if (q.empty()) return false;
        item = std::move(q.front());
        q.pop();
        sz.store(q.size(), std::memory_order_release);
        return true;
    }
    size_t size() const { return sz.load(std::memory_order_acquire); }
    bool empty() const { return size() == 0; }
    bool full()  const { return size() >= cap; }
};

// ============================================================================
// 2. RING BUFFER QUEUE (Lock‑free, CAS‑based, bounded)
// ============================================================================
template<typename T>
class RingBufferQueue {
    struct Slot {
        alignas(64) std::atomic<bool> occupied{false};
        T data;
        Slot() = default;
        Slot(const Slot&) = delete;
        Slot& operator=(const Slot&) = delete;
    };
    Slot* buf;
    alignas(64) std::atomic<size_t> write_pos{0};
    alignas(64) std::atomic<size_t> read_pos{0};
    size_t cap, mask;
    std::atomic<size_t> sz{0};

public:
    explicit RingBufferQueue(size_t capacity) : buf(nullptr), cap(1) {
        if (!capacity) throw std::invalid_argument("capacity>0");
        while (cap < capacity) cap <<= 1;
        mask = cap - 1;
        buf = static_cast<Slot*>(operator new[](sizeof(Slot) * cap));
        for (size_t i = 0; i < cap; ++i) new (&buf[i]) Slot();
    }
    ~RingBufferQueue() {
        for (size_t i = 0; i < cap; ++i) buf[i].~Slot();
        operator delete[](buf);
    }
    RingBufferQueue(const RingBufferQueue&) = delete;
    RingBufferQueue& operator=(const RingBufferQueue&) = delete;

    bool enqueue(T item) {
        size_t w = write_pos.load(std::memory_order_relaxed);
        while (true) {
            size_t r = read_pos.load(std::memory_order_acquire);
            if (w - r >= cap) return false;
            if (write_pos.compare_exchange_weak(w, w+1, std::memory_order_acq_rel)) {
                size_t idx = w & mask;
                while (buf[idx].occupied.load(std::memory_order_acquire))
                    __builtin_ia32_pause();
                buf[idx].data = std::move(item);
                buf[idx].occupied.store(true, std::memory_order_release);
                sz.fetch_add(1, std::memory_order_release);
                return true;
            }
        }
    }
    bool dequeue(T& item) {
        size_t r = read_pos.load(std::memory_order_relaxed);
        while (true) {
            size_t w = write_pos.load(std::memory_order_acquire);
            if (r >= w) return false;
            if (read_pos.compare_exchange_weak(r, r+1, std::memory_order_acq_rel)) {
                size_t idx = r & mask;
                while (!buf[idx].occupied.load(std::memory_order_acquire))
                    __builtin_ia32_pause();
                item = std::move(buf[idx].data);
                buf[idx].occupied.store(false, std::memory_order_release);
                sz.fetch_sub(1, std::memory_order_release);
                return true;
            }
        }
    }
    size_t size() const { return sz.load(std::memory_order_acquire); }
    bool empty() const { return size() == 0; }
    bool full()  const { return size() >= cap; }
};

// ============================================================================
// 3. BOUNDED MPMC QUEUE (Simplified lock‑free, CAS‑based, bounded)
// ============================================================================
template<typename T>
class BoundedMPMCQueue {
    struct Slot {
        alignas(64) std::atomic<bool> ready{false};
        T data;
        Slot() = default;
        Slot(const Slot&) = delete;
        Slot& operator=(const Slot&) = delete;
    };
    Slot* buf;
    alignas(64) std::atomic<size_t> head{0};
    alignas(64) std::atomic<size_t> tail{0};
    size_t cap, mask;
    std::atomic<size_t> sz{0};

public:
    explicit BoundedMPMCQueue(size_t capacity) : buf(nullptr), cap(1) {
        if (!capacity) throw std::invalid_argument("capacity>0");
        while (cap < capacity) cap <<= 1;
        mask = cap - 1;
        buf = static_cast<Slot*>(operator new[](sizeof(Slot) * cap));
        for (size_t i = 0; i < cap; ++i) new (&buf[i]) Slot();
    }
    ~BoundedMPMCQueue() {
        for (size_t i = 0; i < cap; ++i) buf[i].~Slot();
        operator delete[](buf);
    }
    BoundedMPMCQueue(const BoundedMPMCQueue&) = delete;
    BoundedMPMCQueue& operator=(const BoundedMPMCQueue&) = delete;

    bool enqueue(T item) {
        size_t t = tail.load(std::memory_order_acquire);
        while (true) {
            size_t h = head.load(std::memory_order_acquire);
            if (t - h >= cap) return false;
            if (tail.compare_exchange_weak(t, t+1, std::memory_order_acq_rel)) {
                size_t idx = t & mask;
                while (buf[idx].ready.load(std::memory_order_acquire))
                    __builtin_ia32_pause();
                buf[idx].data = std::move(item);
                buf[idx].ready.store(true, std::memory_order_release);
                sz.fetch_add(1, std::memory_order_release);
                return true;
            }
        }
    }
    bool dequeue(T& item) {
        size_t h = head.load(std::memory_order_acquire);
        while (true) {
            size_t t = tail.load(std::memory_order_acquire);
            if (h >= t) return false;
            if (head.compare_exchange_weak(h, h+1, std::memory_order_acq_rel)) {
                size_t idx = h & mask;
                while (!buf[idx].ready.load(std::memory_order_acquire))
                    __builtin_ia32_pause();
                item = std::move(buf[idx].data);
                buf[idx].ready.store(false, std::memory_order_release);
                sz.fetch_sub(1, std::memory_order_release);
                return true;
            }
        }
    }
    size_t size() const { return sz.load(std::memory_order_acquire); }
    bool empty() const { return size() == 0; }
    bool full()  const { return size() >= cap; }
};

// ============================================================================
// 4. MICHAEL-SCOTT QUEUE WITH HAZARD POINTERS (Unbounded, lock‑free)
// ============================================================================

class HazardManager {
    static constexpr size_t MAX_THREADS = 256;
    static constexpr size_t RETIRE_DELAY = 100;
    struct HP { std::atomic<void*> ptr{nullptr}; std::atomic<bool> active{false}; };
    struct Retired { void* node; std::function<void(void*)> deleter; uint64_t epoch; };
    std::vector<HP> hps;
    std::vector<Retired> retired;
    std::mutex retire_mtx;
    std::atomic<uint64_t> global_epoch{0};
public:
    HazardManager() : hps(MAX_THREADS) {}
    
    void* protect(size_t tid, std::atomic<void*>& src) {
        void* p = src.load(std::memory_order_acquire);
        HP& hp = hps[tid % MAX_THREADS];
        hp.ptr.store(p, std::memory_order_relaxed);
        hp.active.store(true, std::memory_order_release);
        if (src.load(std::memory_order_acquire) != p) {
            hp.ptr.store(nullptr, std::memory_order_relaxed);
            return protect(tid, src);
        }
        return p;
    }
    void release(size_t tid) {
        HP& hp = hps[tid % MAX_THREADS];
        hp.active.store(false, std::memory_order_release);
        hp.ptr.store(nullptr, std::memory_order_relaxed);
    }
    template<typename Node>
    void retire(Node* node) {
        std::lock_guard<std::mutex> lock(retire_mtx);
        retired.push_back({node, [](void* p){ delete static_cast<Node*>(p); }, global_epoch.load()});
        if (retired.size() >= RETIRE_DELAY) scan();
    }
    void scan() {
        std::vector<void*> active;
        for (auto& hp : hps)
            if (hp.active.load(std::memory_order_acquire))
                if (void* p = hp.ptr.load(std::memory_order_relaxed))
                    active.push_back(p);
        std::sort(active.begin(), active.end());
        auto it = retired.begin();
        while (it != retired.end()) {
            if (!std::binary_search(active.begin(), active.end(), it->node)) {
                it->deleter(it->node);
                it = retired.erase(it);
            } else ++it;
        }
        global_epoch.fetch_add(1, std::memory_order_relaxed);
    }
    static HazardManager& instance() { static HazardManager inst; return inst; }
};

template<typename T>
class TwoLockHazardQueue {
    struct Node {
        std::shared_ptr<T> data;
        std::atomic<Node*> next;
        Node() : next(nullptr) {}
        Node(T&& val) : data(std::make_shared<T>(std::move(val))), next(nullptr) {}
    };
    std::atomic<Node*> head;
    std::atomic<Node*> tail;
    std::atomic<size_t> sz{0};

    static size_t thread_id() {
        static std::atomic<size_t> counter{0};
        static thread_local size_t id = counter.fetch_add(1);
        return id;
    }

public:
    explicit TwoLockHazardQueue(size_t /*capacity*/ = 0) {
        Node* dummy = new Node();
        head.store(dummy, std::memory_order_relaxed);
        tail.store(dummy, std::memory_order_relaxed);
    }
    ~TwoLockHazardQueue() {
        T dummy;
        while (dequeue(dummy)) {}
        Node* cur = head.load();
        while (cur) { Node* nxt = cur->next.load(); delete cur; cur = nxt; }
    }
    TwoLockHazardQueue(const TwoLockHazardQueue&) = delete;
    TwoLockHazardQueue& operator=(const TwoLockHazardQueue&) = delete;

    bool enqueue(T item) {
        Node* node = new Node(std::move(item));
        while (true) {
            Node* t = tail.load(std::memory_order_acquire);
            Node* nxt = t->next.load(std::memory_order_acquire);
            if (t != tail.load(std::memory_order_acquire)) continue;
            if (nxt) {
                tail.compare_exchange_weak(t, nxt, std::memory_order_release, std::memory_order_relaxed);
                continue;
            }
            if (t->next.compare_exchange_weak(nxt, node, std::memory_order_release, std::memory_order_relaxed)) {
                tail.compare_exchange_weak(t, node, std::memory_order_release, std::memory_order_relaxed);
                sz.fetch_add(1, std::memory_order_release);
                return true;
            }
        }
    }

    bool dequeue(T& item) {
        size_t tid = thread_id();
        while (true) {
            Node* h = head.load(std::memory_order_acquire);
            HazardManager::instance().protect(tid, reinterpret_cast<std::atomic<void*>&>(head));
            if (h != head.load(std::memory_order_acquire)) {
                HazardManager::instance().release(tid);
                continue;
            }
            Node* t = tail.load(std::memory_order_acquire);
            Node* nxt = h->next.load(std::memory_order_acquire);
            if (h == t) {
                HazardManager::instance().release(tid);
                if (!nxt) return false;
                tail.compare_exchange_weak(t, nxt, std::memory_order_release, std::memory_order_relaxed);
                continue;
            }
            if (nxt && nxt->data) item = std::move(*nxt->data);
            if (head.compare_exchange_weak(h, nxt, std::memory_order_release, std::memory_order_relaxed)) {
                HazardManager::instance().release(tid);
                HazardManager::instance().retire(h);
                sz.fetch_sub(1, std::memory_order_release);
                return true;
            }
            HazardManager::instance().release(tid);
        }
    }
    size_t size() const { return sz.load(std::memory_order_acquire); }
    bool empty() const { return size() == 0; }
    bool full() const { return false; }
};

#endif // QUEUES_H
