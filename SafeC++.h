#pragma once

#include <atomic>
#include <cassert>
#include <optional>
#include <shared_mutex>
#include <type_traits>
#include <utility>

namespace safe {

template<typename T, typename M = std::shared_mutex>
class SharedPtr;

template<typename T, typename M = std::shared_mutex>
class WeakPtr;

template<typename T, typename M = std::shared_mutex>
class CopyOnWritePtr;

template<typename T, typename M>
class [[nodiscard]] SharedLockGuard
{
    const T *m_data;
    M *m_mutex;
    bool m_ownsLock;

public:
    SharedLockGuard(const T *data, M *mutex)
        : m_data(data)
        , m_mutex(mutex)
        , m_ownsLock(true)
    {
        m_mutex->lock_shared();
    }

    ~SharedLockGuard()
    {
        if (m_ownsLock)
            m_mutex->unlock_shared();
    }

    SharedLockGuard(SharedLockGuard &&other) noexcept
        : m_data(other.m_data)
        , m_mutex(other.m_mutex)
        , m_ownsLock(other.m_ownsLock)
    {
        other.m_ownsLock = false;
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
    bool m_ownsLock;

public:
    UniqueLockGuard(T *data, M *mutex)
        : m_data(data)
        , m_mutex(mutex)
        , m_ownsLock(true)
    {
        m_mutex->lock();
    }

    ~UniqueLockGuard()
    {
        if (m_ownsLock)
            m_mutex->unlock();
    }

    UniqueLockGuard(UniqueLockGuard &&other) noexcept
        : m_data(other.m_data)
        , m_mutex(other.m_mutex)
        , m_ownsLock(other.m_ownsLock)
    {
        other.m_ownsLock = false;
    }

    UniqueLockGuard(const UniqueLockGuard &) = delete;
    UniqueLockGuard &operator=(const UniqueLockGuard &) = delete;

    const T *operator->() const { return m_data; }
    const T &operator*() const { return *m_data; }

    T *operator->() { return m_data; }
    T &operator*() { return *m_data; }
};

namespace __Internal__ {
// Tags selecting the cast performed by the pointer converting constructors
struct StaticCastTag
{};
struct DynamicCastTag
{};

struct ControlBlockBase
{
    std::atomic<size_t> sharedRefs{1};
    std::atomic<size_t> weakRefs{1}; // 1 implicit count held collectively by shared_refs

    virtual ~ControlBlockBase() = default;
    virtual void destroyData() noexcept = 0;
    virtual void destroySelf() noexcept = 0;

    void addShared() noexcept { sharedRefs.fetch_add(1, std::memory_order_relaxed); }

    void releaseShared() noexcept
    {
        // If we are the last shared owner
        if (sharedRefs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            destroyData(); // Destroy T immediately
            releaseWeak(); // Decrement the implicit weak ref
        }
    }

    void addWeak() noexcept { weakRefs.fetch_add(1, std::memory_order_relaxed); }

    void releaseWeak() noexcept
    {
        // If we are the absolute last observer (no shared, no weak)
        if (weakRefs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            destroySelf(); // Free the Control Block memory itself
        }
    }

    bool tryAddShared() noexcept
    {
        size_t count = sharedRefs.load(std::memory_order_relaxed);
        while (count != 0) {
            if (sharedRefs.compare_exchange_weak(count,
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

    void destroyData() noexcept override { data.reset(); }

    void destroySelf() noexcept override { delete this; }
};
} // namespace __Internal__

template<typename T, typename M>
class SharedPtr
{
    template<typename, typename>
    friend class SharedPtr;

    template<typename, typename>
    friend class WeakPtr;

protected:
    __Internal__::ControlBlockBase *m_cb{nullptr};
    T *m_data{nullptr};
    M *m_mutex{nullptr};

    void release() noexcept
    {
        if (m_cb) {
            m_cb->releaseShared();
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
            m_cb->addShared();
    }

    SharedPtr(SharedPtr &&other) noexcept
        : m_cb(std::exchange(other.m_cb, nullptr))
        , m_data(std::exchange(other.m_data, nullptr))
        , m_mutex(std::exchange(other.m_mutex, nullptr))
    {}

    // Implicit conversion to a compatible pointer type (e.g. Derived -> Base)
    template<typename U>
        requires(!std::is_same_v<U, T> && std::is_convertible_v<U *, T *>)
    SharedPtr(const SharedPtr<U, M> &other) noexcept
        : m_cb(other.m_cb)
        , m_data(static_cast<T *>(other.m_data))
        , m_mutex(other.m_mutex)
    {
        if (m_cb)
            m_cb->addShared();
    }

    template<typename U>
        requires(!std::is_same_v<U, T> && std::is_convertible_v<U *, T *>)
    SharedPtr(SharedPtr<U, M> &&other) noexcept
        : m_cb(std::exchange(other.m_cb, nullptr))
        , m_data(static_cast<T *>(std::exchange(other.m_data, nullptr)))
        , m_mutex(std::exchange(other.m_mutex, nullptr))
    {}

    // Used by safe::staticCast(), shares ownership with other
    template<typename U>
    SharedPtr(const SharedPtr<U, M> &other, __Internal__::StaticCastTag) noexcept
        : m_cb(other.m_cb)
        , m_data(static_cast<T *>(other.m_data))
        , m_mutex(other.m_mutex)
    {
        if (m_cb)
            m_cb->addShared();
    }

    // Used by safe::dynamicCast(), empty if the cast fails
    template<typename U>
    SharedPtr(const SharedPtr<U, M> &other, __Internal__::DynamicCastTag) noexcept
        : m_data(dynamic_cast<T *>(other.m_data))
    {
        if (m_data) {
            m_cb = other.m_cb;
            m_mutex = other.m_mutex;
            if (m_cb)
                m_cb->addShared();
        }
    }

    virtual ~SharedPtr() { release(); }

    SharedPtr &operator=(const SharedPtr &other) noexcept
    {
        if (this != &other) {
            release();
            m_cb = other.m_cb;
            m_data = other.m_data;
            m_mutex = other.m_mutex;
            if (m_cb)
                m_cb->addShared();
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

    template<typename U>
        requires(!std::is_same_v<U, T> && std::is_convertible_v<U *, T *>)
    SharedPtr &operator=(const SharedPtr<U, M> &other) noexcept
    {
        release();
        m_cb = other.m_cb;
        m_data = static_cast<T *>(other.m_data);
        m_mutex = other.m_mutex;
        if (m_cb)
            m_cb->addShared();
        return *this;
    }

    template<typename U>
        requires(!std::is_same_v<U, T> && std::is_convertible_v<U *, T *>)
    SharedPtr &operator=(SharedPtr<U, M> &&other) noexcept
    {
        release();
        m_cb = std::exchange(other.m_cb, nullptr);
        m_data = static_cast<T *>(std::exchange(other.m_data, nullptr));
        m_mutex = std::exchange(other.m_mutex, nullptr);
        return *this;
    }

    __Internal__::ControlBlockBase *controlBlock() const noexcept { return m_cb; }

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

// std::static_pointer_cast() equivalent
template<typename T, typename U, typename M>
[[nodiscard]] SharedPtr<T, M> staticCast(const SharedPtr<U, M> &other) noexcept
{
    return SharedPtr<T, M>(other, __Internal__::StaticCastTag{});
}

// std::dynamic_pointer_cast() equivalent, returns an empty pointer if the cast fails
template<typename T, typename U, typename M>
[[nodiscard]] SharedPtr<T, M> dynamicCast(const SharedPtr<U, M> &other) noexcept
{
    return SharedPtr<T, M>(other, __Internal__::DynamicCastTag{});
}

template<typename T, typename M>
class WeakPtr
{
    template<typename, typename>
    friend class WeakPtr;

private:
    __Internal__::ControlBlockBase *m_cb{nullptr};
    T *m_data{nullptr};
    M *m_mutex{nullptr};

    void release() noexcept
    {
        if (m_cb) {
            m_cb->releaseWeak();
            m_cb = nullptr;
            m_data = nullptr;
            m_mutex = nullptr;
        }
    }

public:
    WeakPtr() = default;

    template<typename U>
        requires std::is_convertible_v<U *, T *>
    WeakPtr(const SharedPtr<U, M> &shared) noexcept
        : m_cb(shared.m_cb)
        , m_data(static_cast<T *>(shared.m_data))
        , m_mutex(shared.m_mutex)
    {
        if (m_cb)
            m_cb->addWeak();
    }

    WeakPtr(const WeakPtr &other) noexcept
        : m_cb(other.m_cb)
        , m_data(other.m_data)
        , m_mutex(other.m_mutex)
    {
        if (m_cb)
            m_cb->addWeak();
    }

    // Implicit conversion to a compatible pointer type (e.g. Derived -> Base)
    template<typename U>
        requires(!std::is_same_v<U, T> && std::is_convertible_v<U *, T *>)
    WeakPtr(const WeakPtr<U, M> &other) noexcept
        : m_cb(other.m_cb)
        , m_data(static_cast<T *>(other.m_data))
        , m_mutex(other.m_mutex)
    {
        if (m_cb)
            m_cb->addWeak();
    }

    WeakPtr(WeakPtr &&other) noexcept
        : m_cb(std::exchange(other.m_cb, nullptr))
        , m_data(std::exchange(other.m_data, nullptr))
        , m_mutex(std::exchange(other.m_mutex, nullptr))
    {}

    ~WeakPtr() { release(); }

    WeakPtr &operator=(const WeakPtr &other) noexcept
    {
        if (this != &other) {
            release();
            m_cb = other.m_cb;
            m_data = other.m_data;
            m_mutex = other.m_mutex;
            if (m_cb)
                m_cb->addWeak();
        }
        return *this;
    }

    template<typename U>
        requires(!std::is_same_v<U, T> && std::is_convertible_v<U *, T *>)
    WeakPtr &operator=(const WeakPtr<U, M> &other) noexcept
    {
        release();
        m_cb = other.m_cb;
        m_data = static_cast<T *>(other.m_data);
        m_mutex = other.m_mutex;
        if (m_cb)
            m_cb->addWeak();
        return *this;
    }

    template<typename U>
        requires std::is_convertible_v<U *, T *>
    WeakPtr &operator=(const SharedPtr<U, M> &shared) noexcept
    {
        release();
        m_cb = shared.m_cb;
        m_data = static_cast<T *>(shared.m_data);
        m_mutex = shared.m_mutex;
        if (m_cb)
            m_cb->addWeak();
        return *this;
    }

    bool expired() const noexcept
    {
        return !m_cb || m_cb->sharedRefs.load(std::memory_order_relaxed) == 0;
    }

    SharedPtr<T, M> lock() const noexcept
    {
        // The data pointer stays valid as long as a shared owner exists, and
        // tryAddShared() only succeeds while T is still alive.
        if (m_cb && m_cb->tryAddShared())
            return SharedPtr<T, M>(m_cb, m_data, m_mutex);
        return SharedPtr<T, M>{};
    }
};

template<typename T, typename M = std::shared_mutex, typename... Args>
SharedPtr<T, M> makeShared(Args &&...args)
{
    auto *cb = new __Internal__::ControlBlock<T, M>(std::forward<Args>(args)...);
    return SharedPtr<T, M>(cb, &(*cb->data), &cb->mutex);
}

template<typename T, typename M>
class CopyOnWritePtr : public SharedPtr<T, M>
{
private:
    // Note: T is what gets cloned, so holding a base pointer to a derived object
    // (e.g. after an up-cast) slices the object on the first detach().
    void detach()
    {
        if (!this->m_cb)
            return;

        if (this->m_cb->sharedRefs.load(std::memory_order_acquire) > 1) {
            // Read-Lock old data to protect copy constructor execution
            auto oldDataGuard = this->sharedLock();

            // Create cloned block
            auto *newCb = new __Internal__::ControlBlock<T, M>(*oldDataGuard);

            this->release();

            this->m_cb = newCb;
            this->m_data = &(*newCb->data);
            this->m_mutex = &newCb->mutex;
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
CopyOnWritePtr<T, M> makeCopyOnWrite(Args &&...args)
{
    auto *cb = new __Internal__::ControlBlock<T, M>(std::forward<Args>(args)...);
    return CopyOnWritePtr<T, M>(cb, &(*cb->data), &cb->mutex);
}

} // namespace Safe
