/*
MIT No Attribution

Copyright 2026, BogDan Vatra <bogdan@kde.org>

Permission is hereby granted, free of charge, to any person obtaining a copy of this
software and associated documentation files (the "Software"), to deal in the Software
without restriction, including without limitation the rights to use, copy, modify,
merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#pragma once

#include <algorithm>
#include <atomic>
#include <cassert>
#include <compare>
#include <concepts>
#include <cstddef>
#include <optional>
#include <shared_mutex>
#include <tuple>
#include <type_traits>
#include <utility>

namespace safe {

// The lock type used by the pointers. std::shared_mutex satisfies it, std::mutex
// does not, and saying so here keeps the diagnostic at the point of use.
template<typename M>
concept SharedMutex = requires(M &mutex) {
    mutex.lock();
    mutex.unlock();
    mutex.lock_shared();
    mutex.unlock_shared();
};

template<typename T, SharedMutex M = std::shared_mutex>
class SharedPtr;

template<typename T, SharedMutex M = std::shared_mutex>
class WeakPtr;

template<typename T, SharedMutex M = std::shared_mutex>
class CopyOnWritePtr;

template<typename T, SharedMutex M = std::shared_mutex>
class EnableSharedFromThis;

template<typename T, SharedMutex M = std::shared_mutex, typename... Args>
SharedPtr<T, M> makeShared(Args &&...args);

template<typename T, SharedMutex M = std::shared_mutex, typename... Args>
CopyOnWritePtr<T, M> makeCopyOnWrite(Args &&...args);

namespace detail {
// Tags selecting the cast performed by the pointer converting constructors
struct StaticCastTag
{};
struct DynamicCastTag
{};
struct ConstCastTag
{};

template<SharedMutex M>
struct ControlBlockBase
{
    // The mutex lives in the erased base, so the pointers only need to remember
    // the block and the (possibly adjusted) object address
    mutable M mutex;

    std::atomic<std::size_t> sharedRefs{1};
    std::atomic<std::size_t> weakRefs{1}; // 1 implicit count held collectively by shared_refs

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
        std::size_t count = sharedRefs.load(std::memory_order_relaxed);
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

template<typename T, SharedMutex M>
struct ControlBlock : public ControlBlockBase<M>
{
    std::optional<T> data; // std::optional allows destroying T while keeping ControlBlock

    template<typename... Args>
    explicit ControlBlock(Args &&...args)
        : data(std::in_place, std::forward<Args>(args)...)
    {}

    void destroyData() noexcept override { data.reset(); }

    void destroySelf() noexcept override { delete this; }
};
} // namespace detail

// Both guards keep the control block alive for as long as they exist, so a guard
// stays valid even if the pointer it came from is reassigned or destroyed while
// the lock is held. They can only be created by the pointer classes.
template<typename T, SharedMutex M>
class [[nodiscard]] SharedLockGuard
{
    template<typename, SharedMutex>
    friend class SharedPtr;

    const T *m_data;
    detail::ControlBlockBase<M> *m_cb;

    SharedLockGuard(detail::ControlBlockBase<M> *cb, const T *data)
        : m_data(data)
        , m_cb(cb)
    {
        // Lock first: if it throws the destructor never runs, so the reference
        // must not be taken yet. The creating pointer keeps the block alive here.
        m_cb->mutex.lock_shared();
        m_cb->addShared();
    }

public:
    ~SharedLockGuard()
    {
        // Unlock before dropping the reference: releasing the last one destroys
        // the control block, and with it the mutex
        if (m_cb) {
            m_cb->mutex.unlock_shared();
            m_cb->releaseShared();
        }
    }

    SharedLockGuard(SharedLockGuard &&other) noexcept
        : m_data(std::exchange(other.m_data, nullptr))
        , m_cb(std::exchange(other.m_cb, nullptr))
    {}

    SharedLockGuard(const SharedLockGuard &) = delete;
    SharedLockGuard &operator=(const SharedLockGuard &) = delete;

    const T *operator->() const { return m_data; }
    const T &operator*() const { return *m_data; }
};

template<typename T, SharedMutex M>
class [[nodiscard]] UniqueLockGuard
{
    template<typename, SharedMutex>
    friend class SharedPtr;

    T *m_data;
    detail::ControlBlockBase<M> *m_cb;

    UniqueLockGuard(detail::ControlBlockBase<M> *cb, T *data)
        : m_data(data)
        , m_cb(cb)
    {
        // Lock first: if it throws the destructor never runs, so the reference
        // must not be taken yet. The creating pointer keeps the block alive here.
        m_cb->mutex.lock();
        m_cb->addShared();
    }

public:
    ~UniqueLockGuard()
    {
        // Unlock before dropping the reference: releasing the last one destroys
        // the control block, and with it the mutex
        if (m_cb) {
            m_cb->mutex.unlock();
            m_cb->releaseShared();
        }
    }

    UniqueLockGuard(UniqueLockGuard &&other) noexcept
        : m_data(std::exchange(other.m_data, nullptr))
        , m_cb(std::exchange(other.m_cb, nullptr))
    {}

    UniqueLockGuard(const UniqueLockGuard &) = delete;
    UniqueLockGuard &operator=(const UniqueLockGuard &) = delete;

    const T *operator->() const { return m_data; }
    const T &operator*() const { return *m_data; }

    T *operator->() { return m_data; }
    T &operator*() { return *m_data; }
};


template<typename T, SharedMutex M>
class SharedPtr
{
    template<typename, SharedMutex>
    friend class SharedPtr;

    template<typename, SharedMutex>
    friend class WeakPtr;

    template<typename, SharedMutex>
    friend class CopyOnWritePtr;

    template<typename U, SharedMutex Mx, typename... Args>
    friend SharedPtr<U, Mx> makeShared(Args &&...args);

    template<typename...>
    friend class ScopedLock;

    // Private, not protected: a protected control block would let any derived class
    // forge a pointer or drop a reference. The friends above are the only holders.
private:
    detail::ControlBlockBase<M> *m_cb{nullptr};
    T *m_data{nullptr};

    // Adopts an existing reference, it does not take one. Only the factories,
    // WeakPtr::lock() and CopyOnWritePtr can hand out a control block.
    SharedPtr(detail::ControlBlockBase<M> *cb, T *data) noexcept
        : m_cb(cb)
        , m_data(data)
    {}

    void release() noexcept
    {
        if (m_cb) {
            m_cb->releaseShared();
            m_cb = nullptr;
            m_data = nullptr;
        }
    }

public:
    using ElementType = T;
    using MutexType = M;

    SharedPtr() = default;

    SharedPtr(std::nullptr_t) noexcept {}

    SharedPtr(const SharedPtr &other) noexcept
        : m_cb(other.m_cb)
        , m_data(other.m_data)
    {
        if (m_cb)
            m_cb->addShared();
    }

    SharedPtr(SharedPtr &&other) noexcept
        : m_cb(std::exchange(other.m_cb, nullptr))
        , m_data(std::exchange(other.m_data, nullptr))
    {}

    // Implicit conversion to a compatible pointer type (e.g. Derived -> Base)
    template<typename U>
        requires(!std::is_same_v<U, T> && std::is_convertible_v<U *, T *>)
    SharedPtr(const SharedPtr<U, M> &other) noexcept
        : m_cb(other.m_cb)
        , m_data(static_cast<T *>(other.m_data))
    {
        if (m_cb)
            m_cb->addShared();
    }

    template<typename U>
        requires(!std::is_same_v<U, T> && std::is_convertible_v<U *, T *>)
    SharedPtr(SharedPtr<U, M> &&other) noexcept
        : m_cb(std::exchange(other.m_cb, nullptr))
        , m_data(static_cast<T *>(std::exchange(other.m_data, nullptr)))
    {}

    // Used by safe::staticCast(), shares ownership with other
    template<typename U>
    SharedPtr(const SharedPtr<U, M> &other, detail::StaticCastTag) noexcept
        : m_cb(other.m_cb)
        , m_data(static_cast<T *>(other.m_data))
    {
        if (m_cb)
            m_cb->addShared();
    }

    // Used by safe::constCast()
    template<typename U>
    SharedPtr(const SharedPtr<U, M> &other, detail::ConstCastTag) noexcept
        : m_cb(other.m_cb)
        , m_data(const_cast<T *>(other.m_data))
    {
        if (m_cb)
            m_cb->addShared();
    }

    // Used by safe::dynamicCast(), empty if the cast fails
    template<typename U>
    SharedPtr(const SharedPtr<U, M> &other, detail::DynamicCastTag) noexcept
        : m_data(dynamic_cast<T *>(other.m_data))
    {
        if (m_data) {
            m_cb = other.m_cb;
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
        return *this;
    }

    SharedPtr &operator=(std::nullptr_t) noexcept
    {
        release();
        return *this;
    }

    void reset() noexcept { release(); }

    void swap(SharedPtr &other) noexcept
    {
        std::swap(m_cb, other.m_cb);
        std::swap(m_data, other.m_data);
    }

    // Ownership comparison, like std::shared_ptr::owner_equal. Two pointers to the
    // same object through different base classes still compare equal.
    template<typename U>
    bool operator==(const SharedPtr<U, M> &other) const noexcept
    {
        return m_cb == other.m_cb;
    }

    template<typename U>
    std::strong_ordering operator<=>(const SharedPtr<U, M> &other) const noexcept
    {
        return std::compare_three_way{}(m_cb, other.m_cb);
    }

    bool operator==(std::nullptr_t) const noexcept { return m_cb == nullptr; }

    // Identity of the owned object, for comparing pointers. Deliberately opaque:
    // handing out the control block would let callers corrupt the reference counts.
    const void *owner() const noexcept { return m_cb; }

    // Number of shared owners, including any live lock guard
    std::size_t useCount() const noexcept
    {
        return m_cb ? m_cb->sharedRefs.load(std::memory_order_relaxed) : 0;
    }

    // Number of observers, plus one collectively held by the shared owners
    std::size_t weakCount() const noexcept
    {
        return m_cb ? m_cb->weakRefs.load(std::memory_order_relaxed) : 0;
    }

    // Explicit Bool Operator to check if valid
    explicit operator bool() const noexcept { return m_cb != nullptr; }

    virtual UniqueLockGuard<T, M> writeLock()
    {
        assert(m_data != nullptr && "SharedPtr is null!");
        return UniqueLockGuard<T, M>(m_cb, m_data);
    }

    virtual UniqueLockGuard<T, M> writeLock() const
    {
        assert(m_data != nullptr && "SharedPtr is null!");
        return UniqueLockGuard<T, M>(m_cb, m_data);
    }

    SharedLockGuard<T, M> readLock() const
    {
        assert(m_data != nullptr && "SharedPtr is null!");
        return SharedLockGuard<T, M>(m_cb, m_data);
    }
};

// std::static_pointer_cast() equivalent
template<typename T, typename U, SharedMutex M>
[[nodiscard]] SharedPtr<T, M> staticCast(const SharedPtr<U, M> &other) noexcept
{
    return SharedPtr<T, M>(other, detail::StaticCastTag{});
}

// std::dynamic_pointer_cast() equivalent, returns an empty pointer if the cast fails
template<typename T, typename U, SharedMutex M>
[[nodiscard]] SharedPtr<T, M> dynamicCast(const SharedPtr<U, M> &other) noexcept
{
    return SharedPtr<T, M>(other, detail::DynamicCastTag{});
}

// std::const_pointer_cast() equivalent
template<typename T, typename U, SharedMutex M>
[[nodiscard]] SharedPtr<T, M> constCast(const SharedPtr<U, M> &other) noexcept
{
    return SharedPtr<T, M>(other, detail::ConstCastTag{});
}

template<typename T, SharedMutex M>
void swap(SharedPtr<T, M> &first, SharedPtr<T, M> &second) noexcept
{
    first.swap(second);
}

template<typename T, SharedMutex M>
void swap(WeakPtr<T, M> &first, WeakPtr<T, M> &second) noexcept
{
    first.swap(second);
}

template<typename T, SharedMutex M>
class WeakPtr
{
    template<typename, SharedMutex>
    friend class WeakPtr;

private:
    detail::ControlBlockBase<M> *m_cb{nullptr};
    T *m_data{nullptr};

    void release() noexcept
    {
        if (m_cb) {
            m_cb->releaseWeak();
            m_cb = nullptr;
            m_data = nullptr;
        }
    }

public:
    using ElementType = T;
    using MutexType = M;

    WeakPtr() = default;

    template<typename U>
        requires std::is_convertible_v<U *, T *>
    WeakPtr(const SharedPtr<U, M> &shared) noexcept
        : m_cb(shared.m_cb)
        , m_data(static_cast<T *>(shared.m_data))
    {
        if (m_cb)
            m_cb->addWeak();
    }

    WeakPtr(const WeakPtr &other) noexcept
        : m_cb(other.m_cb)
        , m_data(other.m_data)
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
    {
        if (m_cb)
            m_cb->addWeak();
    }

    WeakPtr(WeakPtr &&other) noexcept
        : m_cb(std::exchange(other.m_cb, nullptr))
        , m_data(std::exchange(other.m_data, nullptr))
    {}

    ~WeakPtr() { release(); }

    WeakPtr &operator=(const WeakPtr &other) noexcept
    {
        if (this != &other) {
            release();
            m_cb = other.m_cb;
            m_data = other.m_data;
            if (m_cb)
                m_cb->addWeak();
        }
        return *this;
    }

    WeakPtr &operator=(WeakPtr &&other) noexcept
    {
        if (this != &other) {
            release();
            m_cb = std::exchange(other.m_cb, nullptr);
            m_data = std::exchange(other.m_data, nullptr);
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
        if (m_cb)
            m_cb->addWeak();
        return *this;
    }

    void reset() noexcept { release(); }

    void swap(WeakPtr &other) noexcept
    {
        std::swap(m_cb, other.m_cb);
        std::swap(m_data, other.m_data);
    }

    // Ownership comparison, like SharedPtr. Two observers of the same object
    // compare equal even if they watch it through different base classes, and
    // an expired observer keeps comparing equal to its peers.
    template<typename U>
    bool operator==(const WeakPtr<U, M> &other) const noexcept
    {
        return m_cb == other.m_cb;
    }

    template<typename U>
    std::strong_ordering operator<=>(const WeakPtr<U, M> &other) const noexcept
    {
        return std::compare_three_way{}(m_cb, other.m_cb);
    }

    // Identity of the observed object, comparable with SharedPtr::owner().
    // Stays the same once the object is gone, so an expired observer is still
    // findable in a container.
    const void *owner() const noexcept { return m_cb; }

    std::size_t useCount() const noexcept
    {
        return m_cb ? m_cb->sharedRefs.load(std::memory_order_relaxed) : 0;
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
            return SharedPtr<T, M>(m_cb, m_data);
        return SharedPtr<T, M>{};
    }
};

namespace detail {
template<typename T, SharedMutex M>
void armSharedFromThis(const SharedPtr<T, M> &owner, T *object) noexcept;

template<SharedMutex M, typename U>
EnableSharedFromThis<U, M> *esftBaseOf(const EnableSharedFromThis<U, M> *) noexcept;

template<typename T, typename M>
concept HasSharedFromThis = SharedMutex<M> && requires(T *object) { esftBaseOf<M>(object); };

template<typename P>
struct IsPlainSharedPtr : std::false_type
{};

template<typename T, SharedMutex M>
struct IsPlainSharedPtr<SharedPtr<T, M>> : std::true_type
{};
} // namespace detail

// Locks several pointers at once. The mutexes are always acquired in the same
// global order (by control block address) and repeated pointers are locked once,
// so two threads locking the same objects in opposite argument order cannot
// deadlock and passing the same pointer twice cannot self deadlock.
template<typename... Ptrs>
class [[nodiscard]] ScopedLock
{
    static_assert(sizeof...(Ptrs) > 0, "ScopedLock needs at least one pointer");

    using MutexType = typename std::tuple_element_t<0, std::tuple<Ptrs...>>::MutexType;
    static_assert((std::is_same_v<MutexType, typename Ptrs::MutexType> && ...),
                  "all pointers must use the same mutex type");

    using Block = detail::ControlBlockBase<MutexType>;
    static constexpr std::size_t Count = sizeof...(Ptrs);

    std::tuple<typename Ptrs::ElementType *...> m_data;
    std::array<Block *, Count> m_locked{};
    std::size_t m_lockedCount{0};

public:
    explicit ScopedLock(Ptrs &...pointers)
        : m_data{pointers.m_data...}
        , m_locked{pointers.m_cb...}
    {
        assert(((pointers.m_cb != nullptr) && ...) && "ScopedLock on a null pointer!");

        std::ranges::sort(m_locked);
        m_lockedCount = std::size_t(std::ranges::unique(m_locked).begin() - m_locked.begin());

        // Take every lock first, then the references: if a lock throws, the
        // destructor never runs and must not have anything to undo
        for (std::size_t i = 0; i < m_lockedCount; ++i)
            m_locked[i]->mutex.lock();
        for (std::size_t i = 0; i < m_lockedCount; ++i)
            m_locked[i]->addShared();
    }

    ~ScopedLock()
    {
        for (std::size_t i = m_lockedCount; i-- > 0;) {
            m_locked[i]->mutex.unlock();
            m_locked[i]->releaseShared();
        }
    }

    ScopedLock(const ScopedLock &) = delete;
    ScopedLock &operator=(const ScopedLock &) = delete;

    template<std::size_t I>
    [[nodiscard]] auto &get() const noexcept
    {
        return *std::get<I>(m_data);
    }
};

// CopyOnWritePtr is deliberately not accepted: locking it here would not detach.
// Convert it to a SharedPtr first if a plain shared view is what you want.
template<typename... Ptrs>
    requires(... && detail::IsPlainSharedPtr<std::remove_cv_t<Ptrs>>::value)
[[nodiscard]] ScopedLock<Ptrs...> lockAll(Ptrs &...pointers)
{
    return ScopedLock<Ptrs...>(pointers...);
}

// Lets an object hand out owning pointers to itself. Inside a critical section all
// you have is a T&, and this is the way back to a pointer.
// The returned pointer is *not* locked, and the mutex is not recursive: locking it
// again while still inside the critical section deadlocks.
template<typename T, SharedMutex M>
class EnableSharedFromThis
{
    template<typename U, SharedMutex Mx>
    friend void detail::armSharedFromThis(const SharedPtr<U, Mx> &, U *) noexcept;

    WeakPtr<T, M> m_weakThis;

protected:
    EnableSharedFromThis() = default;

    // A copy is a different object, so it must not inherit the back reference.
    // Whoever takes ownership of the copy arms it.
    EnableSharedFromThis(const EnableSharedFromThis &) noexcept {}
    EnableSharedFromThis &operator=(const EnableSharedFromThis &) noexcept { return *this; }
    ~EnableSharedFromThis() = default;

public:
    [[nodiscard]] SharedPtr<T, M> sharedFromThis() const noexcept { return m_weakThis.lock(); }
    [[nodiscard]] WeakPtr<T, M> weakFromThis() const noexcept { return m_weakThis; }
    [[nodiscard]] SharedPtr<T, M> sharedFromThis() noexcept { return m_weakThis.lock(); }
    [[nodiscard]] WeakPtr<T, M> weakFromThis() noexcept { return m_weakThis; }
};

namespace detail {
template<typename T, SharedMutex M>
void armSharedFromThis(const SharedPtr<T, M> &owner, T *object) noexcept
{
    if constexpr (HasSharedFromThis<T, M>) {
        // EnableSharedFromThis<U, M>, with U deduced from the base rather than from T
        using EsftBase = std::remove_pointer_t<decltype(esftBaseOf<M>(object))>;
        static_cast<EsftBase *>(object)->m_weakThis = owner;
    }
}
} // namespace detail

template<typename T, SharedMutex M, typename... Args>
SharedPtr<T, M> makeShared(Args &&...args)
{
    auto *cb = new detail::ControlBlock<T, M>(std::forward<Args>(args)...);
    SharedPtr<T, M> pointer(cb, &(*cb->data));
    detail::armSharedFromThis(pointer, &(*cb->data));
    return pointer;
}

// A shared pointer that clones the object the first time it is written to while
// other owners exist.
//
// Interaction with WeakPtr: a WeakPtr observes a *control block*, not a pointer.
// Once this pointer detaches it moves to a fresh block, so observers taken before
// the detach keep watching the original object and never see the clone. Take the
// WeakPtr from the pointer you actually want to follow, or use a plain SharedPtr
// if observers must track every write.
template<typename T, SharedMutex M>
class CopyOnWritePtr : public SharedPtr<T, M>
{
    using Base = SharedPtr<T, M>;

    template<typename U, SharedMutex Mx, typename... Args>
    friend CopyOnWritePtr<U, Mx> makeCopyOnWrite(Args &&...args);

    CopyOnWritePtr(detail::ControlBlockBase<M> *cb, T *data) noexcept
        : Base(cb, data)
    {}

    // Note: T is what gets cloned, so holding a base pointer to a derived object
    // (e.g. after an up-cast) slices the object on the first detach().
    void detach()
    {
        if (!this->m_cb)
            return;

        if (this->m_cb->sharedRefs.load(std::memory_order_acquire) > 1) {
            detail::ControlBlock<T, M> *newCb = nullptr;
            {
                // Read-Lock old data to protect copy constructor execution. The guard
                // must be released before release(), which may destroy the old block
                // (and with it the mutex) if the other owners vanish meanwhile.
                auto oldDataGuard = this->readLock();

                // Create cloned block
                newCb = new detail::ControlBlock<T, M>(*oldDataGuard);
            }

            this->release();

            this->m_cb = newCb;
            this->m_data = &(*newCb->data);
            detail::armSharedFromThis(static_cast<const Base &>(*this), this->m_data);
        }
    }

public:
    CopyOnWritePtr() = default;

    // Adopts a shared object. Deliberately not `using Base::Base`: inheriting the
    // converting constructors would let SharedPtr<Derived> become CopyOnWritePtr<Base>
    // implicitly, which slices on the first detach().
    CopyOnWritePtr(const Base &other) noexcept
        : Base(other)
    {}

    UniqueLockGuard<T, M> writeLock() override
    {
        assert(this->m_data != nullptr && "CopyOnWritePtr is null!");
        detach();
        return Base::writeLock();
    }
};

template<typename T, SharedMutex M, typename... Args>
CopyOnWritePtr<T, M> makeCopyOnWrite(Args &&...args)
{
    auto *cb = new detail::ControlBlock<T, M>(std::forward<Args>(args)...);
    CopyOnWritePtr<T, M> pointer(cb, &(*cb->data));
    detail::armSharedFromThis(static_cast<const SharedPtr<T, M> &>(pointer), &(*cb->data));
    return pointer;
}

} // namespace safe

// Hashing is by owner, exactly like ==, so the pointers work as keys in the
// unordered containers. A SharedPtr and a WeakPtr to the same object hash the
// same, and an expired WeakPtr keeps the hash it had.
//
// CopyOnWritePtr is deliberately left unhashable: detaching moves it to a fresh
// control block, so a key already sitting in a container would silently stop
// matching the pointer it was made from. Convert it to a SharedPtr, and mean it,
// if you want to key on the object it owns right now.
namespace std {

template<typename T, safe::SharedMutex M>
struct hash<safe::SharedPtr<T, M>>
{
    std::size_t operator()(const safe::SharedPtr<T, M> &pointer) const noexcept
    {
        return std::hash<const void *>{}(pointer.owner());
    }
};

template<typename T, safe::SharedMutex M>
struct hash<safe::WeakPtr<T, M>>
{
    std::size_t operator()(const safe::WeakPtr<T, M> &pointer) const noexcept
    {
        return std::hash<const void *>{}(pointer.owner());
    }
};

} // namespace std
