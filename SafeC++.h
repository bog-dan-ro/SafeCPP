#pragma once

#include <atomic>
#include <cassert>
#include <optional>
#include <shared_mutex>
#include <utility>

namespace Safe {

template<typename T, typename M>
class [[nodiscard]] SharedLockGuard
{
    const T *m_data;
    M *m_mutex;
    bool m_owns_lock;

public:
    SharedLockGuard(const T *data, M *mutex)
        : m_data(data)
        , m_mutex(mutex)
        , m_owns_lock(true)
    {
        m_mutex->lock_shared();
    }

    ~SharedLockGuard()
    {
        if (m_owns_lock)
            m_mutex->unlock_shared();
    }

    SharedLockGuard(SharedLockGuard &&other) noexcept
        : m_data(other.m_data)
        , m_mutex(other.m_mutex)
        , m_owns_lock(other.m_owns_lock)
    {
        other.m_owns_lock = false;
    }

    SharedLockGuard(const SharedLockGuard &) = delete;
    SharedLockGuard &operator=(const SharedLockGuard &) = delete;

    const T *operator->() const { return m_data; }
    const T &operator*() const { return *m_data; }
};

template<typename T, typename M>
class [[nodiscard]] UniqueLockGuard
{
    T *m_data;
    M *m_mutex;
    bool m_owns_lock;

public:
    UniqueLockGuard(T *data, M *mutex)
        : m_data(data)
        , m_mutex(mutex)
        , m_owns_lock(true)
    {
        m_mutex->lock();
    }

    ~UniqueLockGuard()
    {
        if (m_owns_lock)
            m_mutex->unlock();
    }

    UniqueLockGuard(UniqueLockGuard &&other) noexcept
        : m_data(other.m_data)
        , m_mutex(other.m_mutex)
        , m_owns_lock(other.m_owns_lock)
    {
        other.m_owns_lock = false;
    }

    UniqueLockGuard(const UniqueLockGuard &) = delete;
    UniqueLockGuard &operator=(const UniqueLockGuard &) = delete;

    const T *operator->() const { return m_data; }
    const T &operator*() const { return *m_data; }

    T *operator->() { return m_data; }
    T &operator*() { return *m_data; }
};

namespace __Internal__ {
struct ControlBlockBase
{
    std::atomic<size_t> shared_refs{1};
    std::atomic<size_t> weak_refs{1}; // 1 implicit count held collectively by shared_refs

    virtual ~ControlBlockBase() = default;
    virtual void destroy_data() noexcept = 0;
    virtual void destroy_self() noexcept = 0;

    void add_shared() noexcept { shared_refs.fetch_add(1, std::memory_order_relaxed); }

    void release_shared() noexcept
    {
        // If we are the last shared owner
        if (shared_refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            destroy_data(); // Destroy T immediately
            release_weak(); // Decrement the implicit weak ref
        }
    }

    void add_weak() noexcept { weak_refs.fetch_add(1, std::memory_order_relaxed); }

    void release_weak() noexcept
    {
        // If we are the absolute last observer (no shared, no weak)
        if (weak_refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            destroy_self(); // Free the Control Block memory itself
        }
    }

    bool try_add_shared() noexcept
    {
        size_t count = shared_refs.load(std::memory_order_relaxed);
        while (count != 0) {
            if (shared_refs.compare_exchange_weak(count,
                                                  count + 1,
                                                  std::memory_order_acquire,
                                                  std::memory_order_relaxed)) {
                return true;
            }
        }
        return false;
    }
};

template<typename T, typename M>
struct ControlBlock : public ControlBlockBase
{
    mutable M mutex;
    std::optional<T> data; // std::optional allows destroying T while keeping ControlBlock

    template<typename... Args>
    explicit ControlBlock(Args &&...args)
        : data(std::in_place, std::forward<Args>(args)...)
    {}

    void destroy_data() noexcept override { data.reset(); }

    void destroy_self() noexcept override { delete this; }
};
} // namespace __Internal__

template<typename T, typename M = std::shared_mutex>
class SharedPtr
{
protected:
    __Internal__::ControlBlockBase *m_cb{nullptr};
    T *m_data{nullptr};
    M *m_mutex{nullptr};

    void release() noexcept
    {
        if (m_cb) {
            m_cb->release_shared();
            m_cb = nullptr;
            m_data = nullptr;
            m_mutex = nullptr;
        }
    }

public:
    SharedPtr() = default;

    SharedPtr(__Internal__::ControlBlockBase *cb, T *data, M *mutex)
        : m_cb(cb)
        , m_data(data)
        , m_mutex(mutex)
    {}

    SharedPtr(const SharedPtr &other) noexcept
        : m_cb(other.m_cb)
        , m_data(other.m_data)
        , m_mutex(other.m_mutex)
    {
        if (m_cb)
            m_cb->add_shared();
    }

    SharedPtr(SharedPtr &&other) noexcept
        : m_cb(std::exchange(other.m_cb, nullptr))
        , m_data(std::exchange(other.m_data, nullptr))
        , m_mutex(std::exchange(other.m_mutex, nullptr))
    {}

    virtual ~SharedPtr() { release(); }

    SharedPtr &operator=(const SharedPtr &other) noexcept
    {
        if (this != &other) {
            release();
            m_cb = other.m_cb;
            m_data = other.m_data;
            m_mutex = other.m_mutex;
            if (m_cb)
                m_cb->add_shared();
        }
        return *this;
    }

    SharedPtr &operator=(SharedPtr &&other) noexcept
    {
        if (this != &other) {
            release();
            m_cb = std::exchange(other.m_cb, nullptr);
            m_data = std::exchange(other.m_data, nullptr);
            m_mutex = std::exchange(other.m_mutex, nullptr);
        }
        return *this;
    }

    __Internal__::ControlBlockBase *get_control_block() const noexcept { return m_cb; }

    // Explicit Bool Operator to check if valid
    explicit operator bool() const noexcept { return m_cb != nullptr; }

    UniqueLockGuard<T, M> lock()
    {
        assert(m_data != nullptr && "SharedPtr is null!");
        return UniqueLockGuard<T, M>(m_data, m_mutex);
    }

    SharedLockGuard<T, M> sharedLock() const
    {
        assert(m_data != nullptr && "SharedPtr is null!");
        return SharedLockGuard<T, M>(m_data, m_mutex);
    }
};
template<typename T, typename M = std::shared_mutex>
class WeakPtr
{
private:
    __Internal__::ControlBlockBase *m_cb{nullptr};

    void release() noexcept
    {
        if (m_cb) {
            m_cb->release_weak();
            m_cb = nullptr;
        }
    }

public:
    WeakPtr() = default;

    WeakPtr(const SharedPtr<T, M> &shared) noexcept
        : m_cb(shared.get_control_block())
    {
        if (m_cb)
            m_cb->add_weak();
    }

    WeakPtr(const WeakPtr &other) noexcept
        : m_cb(other.m_cb)
    {
        if (m_cb)
            m_cb->add_weak();
    }

    WeakPtr(WeakPtr &&other) noexcept
        : m_cb(std::exchange(other.m_cb, nullptr))
    {}

    ~WeakPtr() { release(); }

    WeakPtr &operator=(const WeakPtr &other) noexcept
    {
        if (this != &other) {
            release();
            m_cb = other.m_cb;
            if (m_cb)
                m_cb->add_weak();
        }
        return *this;
    }

    WeakPtr &operator=(const SharedPtr<T, M> &shared) noexcept
    {
        release();
        m_cb = shared.get_control_block();
        if (m_cb)
            m_cb->add_weak();
        return *this;
    }

    bool expired() const noexcept
    {
        return !m_cb || m_cb->shared_refs.load(std::memory_order_relaxed) == 0;
    }

    SharedPtr<T, M> lock() const noexcept
    {
        if (m_cb && m_cb->try_add_shared()) {
            auto *concrete_cb = static_cast<__Internal__::ControlBlock<T, M> *>(m_cb);
            return SharedPtr<T, M>(m_cb, &(*concrete_cb->data), &concrete_cb->mutex);
        }
        return SharedPtr<T, M>{};
    }
};

template<typename T, typename M = std::shared_mutex, typename... Args>
SharedPtr<T, M> MakeShared(Args &&...args)
{
    auto *cb = new __Internal__::ControlBlock<T, M>(std::forward<Args>(args)...);
    return SharedPtr<T, M>(cb, &(*cb->data), &cb->mutex);
}

template<typename T, typename M = std::shared_mutex>
class CopyOnWritePtr : public SharedPtr<T, M>
{
private:
    void detach()
    {
        if (!this->m_cb)
            return;

        if (this->m_cb->shared_refs.load(std::memory_order_acquire) > 1) {
            // Read-Lock old data to protect copy constructor execution
            auto old_data_guard = this->sharedLock();

            // Create cloned block
            auto *new_cb = new __Internal__::ControlBlock<T, M>(*old_data_guard);

            this->release();

            this->m_cb = new_cb;
            this->m_data = &(*new_cb->data);
            this->m_mutex = &new_cb->mutex;
        }
    }

public:
    using SharedPtr<T, M>::SharedPtr;

    CopyOnWritePtr(const SharedPtr<T, M> &other) noexcept
        : SharedPtr<T, M>(other)
    {}

    SharedLockGuard<T, M> sharedLock() const
    {
        assert(this->m_data != nullptr && "CopyOnWritePtr is null!");
        return SharedLockGuard<T, M>(this->m_data, this->m_mutex);
    }

    UniqueLockGuard<T, M> lock()
    {
        assert(this->m_data != nullptr && "CopyOnWritePtr is null!");
        detach();
        return UniqueLockGuard<T, M>(this->m_data, this->m_mutex);
    }
};

template<typename T, typename M = std::shared_mutex, typename... Args>
CopyOnWritePtr<T, M> MakeCopyOnWrite(Args &&...args)
{
    auto *cb = new __Internal__::ControlBlock<T, M>(std::forward<Args>(args)...);
    return CopyOnWritePtr<T, M>(cb, &(*cb->data), &cb->mutex);
}

} // namespace Safe