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
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <shared_mutex>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <type_traits>
#include <typeinfo>
#include <utility>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

// A null pointer where a valid one is needed (locking it, adopting it, aliasing
// with it) is a precondition violation: an assert in debug builds, undefined
// behavior in release builds. Define SAFECPP_THROW_ON_NULL to 1, or turn on the
// CMake option of that name, and safe::BadPointerAccess is thrown instead.
#ifndef SAFECPP_THROW_ON_NULL
#define SAFECPP_THROW_ON_NULL 0
#endif

// Undefined again at the end of this header
#if SAFECPP_THROW_ON_NULL
#define SAFECPP_EXPECT(condition, message) \
    do { \
        if (!(condition)) [[unlikely]] \
            throw ::safe::BadPointerAccess(message); \
    } while (false)
#else
#define SAFECPP_EXPECT(condition, message) assert((condition) && message)
#endif

namespace safe {

// See SAFECPP_THROW_ON_NULL. Declared outside of the namespace below, so it is the
// same type whichever way the code that throws it was built.
class BadPointerAccess : public std::logic_error
{
public:
    using std::logic_error::logic_error;
};

#if SAFECPP_THROW_ON_NULL
// Everything else is a different entity from the one of the default build. Code
// built with and without the option can be linked into one program without
// breaking the one definition rule, and a pointer handed from one side to the
// other is a link error rather than a silent mix of both.
inline namespace throwOnNull {
#endif

// The lock type used by the pointers. std::shared_mutex satisfies it, std::mutex
// does not, and saying so here keeps the diagnostic at the point of use.
template<typename M>
concept SharedMutex = requires(M &mutex) {
    mutex.lock();
    mutex.unlock();
    mutex.lock_shared();
    mutex.unlock_shared();
};

namespace detail {
// Tells the CPU that this is a busy-wait, so it can give the other hyper-thread
// a turn and leave the loop without a mis-speculation penalty
inline void cpuPause() noexcept
{
#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
    _mm_pause();
#elif defined(_MSC_VER) && (defined(_M_ARM) || defined(_M_ARM64))
    __yield();
#elif defined(__i386__) || defined(__x86_64__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || (defined(__arm__) && defined(__ARM_ARCH) && __ARM_ARCH >= 7)
    __asm__ __volatile__("yield");
#endif
}

// The waiting strategy of the spin locks: spin a little longer on every round,
// then start handing the time slice back so a preempted lock holder gets to run
class SpinWait
{
    static constexpr unsigned SpinRounds = 6; // 1 + 2 + ... + 32 pauses

    unsigned m_round{0};

public:
    void operator()() noexcept
    {
        if (m_round < SpinRounds) {
            for (unsigned i = 0; i < (1u << m_round); ++i)
                cpuPause();
            ++m_round;
        } else {
            std::this_thread::yield();
        }
    }
};
} // namespace detail

// The spin locks never put a thread to sleep: a waiter spins, and yields once that
// took too long. That makes them cheaper than a mutex for critical sections of a
// few instructions (a setter, a counter) and much worse for anything longer, where
// the waiters burn the CPU the owner needs. They are not fair and not recursive.
//
// The members use the standard library spelling on purpose, so the locks work with
// std::lock_guard, std::scoped_lock and std::shared_lock.

// Exclusive only, one byte. Not a SharedMutex: it cannot be the M of a pointer.
class SpinLock
{
    std::atomic<bool> m_locked{false};

public:
    constexpr SpinLock() noexcept = default;
    SpinLock(const SpinLock &) = delete;
    SpinLock &operator=(const SpinLock &) = delete;

    void lock() noexcept
    {
        detail::SpinWait wait;
        while (m_locked.exchange(true, std::memory_order_acquire)) {
            // Wait on a plain load: retrying the exchange would keep stealing the
            // cache line from the owner
            while (m_locked.load(std::memory_order_relaxed))
                wait();
        }
    }

    [[nodiscard]] bool try_lock() noexcept
    {
        return !m_locked.load(std::memory_order_relaxed)
               && !m_locked.exchange(true, std::memory_order_acquire);
    }

    void unlock() noexcept { m_locked.store(false, std::memory_order_release); }
};

// Many readers or one writer, four bytes. Satisfies SharedMutex, so it can be the
// M of a pointer: safe::makeShared<T, safe::SharedSpinLock>().
//
// A waiting writer announces itself and from then on no new reader gets in, so a
// steady stream of readers cannot starve the writers. The other way round is not
// guaranteed: writers arriving back to back keep the readers out.
class SharedSpinLock
{
    using State = std::uint32_t;

    static constexpr State Writer = 1;  // a writer owns the lock
    static constexpr State Pending = 2; // a writer is waiting for the readers to leave
    static constexpr State Reader = 4;  // one reader, the count lives in the bits above

    std::atomic<State> m_state{0};

public:
    constexpr SharedSpinLock() noexcept = default;
    SharedSpinLock(const SharedSpinLock &) = delete;
    SharedSpinLock &operator=(const SharedSpinLock &) = delete;

    void lock() noexcept
    {
        detail::SpinWait wait;
        State state = m_state.load(std::memory_order_relaxed);
        for (;;) {
            if ((state & ~Pending) == 0) {
                // Free. Taking it withdraws the announcement as well: it may be
                // ours, and a writer that is still waiting posts its own again.
                if (m_state.compare_exchange_weak(state,
                                                  Writer,
                                                  std::memory_order_acquire,
                                                  std::memory_order_relaxed)) {
                    return;
                }
                continue; // state was reloaded by the failed exchange
            }
            if (!(state & Pending))
                m_state.fetch_or(Pending, std::memory_order_relaxed);
            wait();
            state = m_state.load(std::memory_order_relaxed);
        }
    }

    [[nodiscard]] bool try_lock() noexcept
    {
        // Leaves the announcement of a waiting writer alone, it did not get its turn
        State state = m_state.load(std::memory_order_relaxed);
        return (state & ~Pending) == 0
               && m_state.compare_exchange_strong(state,
                                                  state | Writer,
                                                  std::memory_order_acquire,
                                                  std::memory_order_relaxed);
    }

    void unlock() noexcept { m_state.fetch_and(~Writer, std::memory_order_release); }

    void lock_shared() noexcept
    {
        detail::SpinWait wait;
        State state = m_state.load(std::memory_order_relaxed);
        for (;;) {
            if (!(state & (Writer | Pending))) {
                if (m_state.compare_exchange_weak(state,
                                                  state + Reader,
                                                  std::memory_order_acquire,
                                                  std::memory_order_relaxed)) {
                    return;
                }
                continue; // state was reloaded by the failed exchange
            }
            wait();
            state = m_state.load(std::memory_order_relaxed);
        }
    }

    [[nodiscard]] bool try_lock_shared() noexcept
    {
        State state = m_state.load(std::memory_order_relaxed);
        return !(state & (Writer | Pending))
               && m_state.compare_exchange_strong(state,
                                                  state + Reader,
                                                  std::memory_order_acquire,
                                                  std::memory_order_relaxed);
    }

    void unlock_shared() noexcept { m_state.fetch_sub(Reader, std::memory_order_release); }
};

template<typename T, SharedMutex M = std::shared_mutex>
class SharedPtr;

template<typename T, SharedMutex M = std::shared_mutex>
class WeakPtr;

template<typename T, SharedMutex M = std::shared_mutex>
class CopyOnWritePtr;

template<typename T, SharedMutex M = std::shared_mutex>
class SharedCopyOnWritePtr;

template<typename T, SharedMutex M = std::shared_mutex>
class WeakCopyOnWritePtr;

template<typename T, SharedMutex M = std::shared_mutex>
class EnableSharedFromThis;

template<typename T, SharedMutex M = std::shared_mutex, typename... Args>
SharedPtr<T, M> makeShared(Args &&...args);

template<typename T, SharedMutex M = std::shared_mutex, typename Allocator, typename... Args>
SharedPtr<T, M> allocateShared(const Allocator &allocator, Args &&...args);

template<typename Deleter, typename T, SharedMutex M>
constexpr Deleter *getDeleter(const SharedPtr<T, M> &pointer) noexcept;

template<typename Deleter, typename T, SharedMutex M>
constexpr Deleter *getDeleter(const CopyOnWritePtr<T, M> &pointer) noexcept;

template<typename T, SharedMutex M = std::shared_mutex, typename... Args>
    requires std::copy_constructible<T>
CopyOnWritePtr<T, M> makeCopyOnWrite(Args &&...args);

template<typename T, SharedMutex M = std::shared_mutex, typename Allocator, typename... Args>
    requires std::copy_constructible<T>
CopyOnWritePtr<T, M> allocateCopyOnWrite(const Allocator &allocator, Args &&...args);

template<typename T, SharedMutex M = std::shared_mutex, typename... Args>
    requires std::copy_constructible<T>
SharedCopyOnWritePtr<T, M> makeSharedCopyOnWrite(Args &&...args);

template<typename T, SharedMutex M = std::shared_mutex, typename Allocator, typename... Args>
    requires std::copy_constructible<T>
SharedCopyOnWritePtr<T, M> allocateSharedCopyOnWrite(const Allocator &allocator, Args &&...args);

template<typename T, typename U, SharedMutex M>
[[nodiscard]] constexpr SharedPtr<T, M> staticCast(const SharedPtr<U, M> &other) noexcept;

template<typename T, typename U, SharedMutex M>
[[nodiscard]] constexpr SharedPtr<T, M> dynamicCast(const SharedPtr<U, M> &other) noexcept;

template<typename T, typename U, SharedMutex M>
[[nodiscard]] constexpr SharedPtr<T, M> constCast(const SharedPtr<U, M> &other) noexcept;

// A cast returns the kind of pointer it was given: a SharedCopyOnWritePtr stays
// on its slot and keeps cloning on write
template<typename T, typename U, SharedMutex M>
[[nodiscard]] constexpr SharedCopyOnWritePtr<T, M>
staticCast(const SharedCopyOnWritePtr<U, M> &other) noexcept;

template<typename T, typename U, SharedMutex M>
[[nodiscard]] constexpr SharedCopyOnWritePtr<T, M>
dynamicCast(const SharedCopyOnWritePtr<U, M> &other) noexcept;

template<typename T, typename U, SharedMutex M>
[[nodiscard]] constexpr SharedCopyOnWritePtr<T, M>
constCast(const SharedCopyOnWritePtr<U, M> &other) noexcept;

// A CopyOnWritePtr is not cast at all. The result would have to be a pointer of
// its own, and what a write through that one does is anybody's guess: clone for
// itself and lose the write, or write in place for every owner. Convert first and
// the answer is in the type: to a SharedPtr for a plain shared view, to a
// SharedCopyOnWritePtr for a pointer that clones on write and takes the
// CopyOnWritePtr along.
template<typename T, typename U, SharedMutex M>
void staticCast(const CopyOnWritePtr<U, M> &other) = delete;

template<typename T, typename U, SharedMutex M>
void dynamicCast(const CopyOnWritePtr<U, M> &other) = delete;

template<typename T, typename U, SharedMutex M>
void constCast(const CopyOnWritePtr<U, M> &other) = delete;

namespace detail {
template<SharedMutex M>
struct ControlBlockBase;

template<typename Object, SharedMutex M>
void armSharedFromThis(ControlBlockBase<M> *cb, Object *object) noexcept;

template<typename T>
struct DefaultDelete
{
    void operator()(T *pointer) const noexcept { delete pointer; }
};

template<SharedMutex M>
struct CloneResult
{
    ControlBlockBase<M> *cb{nullptr};
    void *data{nullptr};
};

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
    // Copies the owned object into a fresh block and returns it together with
    // oldData moved over to the copy. Only the blocks created for a CopyOnWritePtr
    // implement it: an empty result means "this block cannot clone", which keeps
    // plain SharedPtr users from having to provide (or pay for) a copy constructor.
    virtual CloneResult<M> clone([[maybe_unused]] const void *oldData) const { return {}; }
    virtual void *getDeleter(const std::type_info &) noexcept { return nullptr; }

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

template<typename T>
class ObjectStorage
{
    alignas(T) std::byte m_storage[sizeof(T)];
    bool m_engaged{false};

    T *raw() noexcept { return reinterpret_cast<T *>(m_storage); }

public:
    T *get() noexcept { return std::launder(raw()); }

    const T *get() const noexcept
    {
        return std::launder(reinterpret_cast<const T *>(m_storage));
    }

    template<typename Allocator, typename... Args>
    void construct(Allocator &allocator, Args &&...args)
    {
        std::allocator_traits<Allocator>::construct(allocator,
                                                    raw(),
                                                    std::forward<Args>(args)...);
        m_engaged = true;
    }

    template<typename Allocator>
    void destroy(Allocator &allocator) noexcept
    {
        if (m_engaged) {
            std::allocator_traits<Allocator>::destroy(allocator, get());
            m_engaged = false;
        }
    }
};

template<typename DerivedBlock, SharedMutex M, typename Allocator>
struct AllocatedControlBlock : ControlBlockBase<M>
{
    using BlockAllocator =
        typename std::allocator_traits<Allocator>::template rebind_alloc<DerivedBlock>;
    using BlockTraits = std::allocator_traits<BlockAllocator>;
    using BlockPointer = typename BlockTraits::pointer;

    [[no_unique_address]] Allocator allocator;
    BlockPointer allocation;

    AllocatedControlBlock(const Allocator &sourceAllocator, BlockPointer sourceAllocation)
        : allocator(sourceAllocator)
        , allocation(sourceAllocation)
    {}

    void destroySelf() noexcept final
    {
        BlockAllocator blockAllocator(allocator);
        BlockPointer blockAllocation = allocation;
        BlockTraits::destroy(blockAllocator, static_cast<DerivedBlock *>(this));
        BlockTraits::deallocate(blockAllocator, blockAllocation, 1);
    }
};

template<typename T, SharedMutex M, typename Allocator, bool Cloneable = false>
struct InplaceControlBlock;

template<typename T, SharedMutex M, typename Allocator, typename... Args>
InplaceControlBlock<T, M, Allocator, true> *
allocateCloneableControlBlock(const Allocator &allocator, Args &&...args);

// True if address lies inside the object root points to
template<typename U>
bool pointsInto(const U *root, const void *address) noexcept
{
    const auto begin = reinterpret_cast<std::uintptr_t>(root);
    const auto value = reinterpret_cast<std::uintptr_t>(address);
    return value >= begin && value - begin < sizeof(U);
}

// Copies *oldRoot into a new cloneable block and translates oldData, which points
// at oldRoot or at one of its subobjects, to the same place inside the copy.
// Returns an empty result if oldData points outside of *oldRoot (an aliasing
// pointer to something the root merely keeps alive): there is nothing in the copy
// it could be translated to.
template<typename U, SharedMutex M, typename Allocator>
CloneResult<M> cloneObject(const Allocator &allocator, const U *oldRoot, const void *oldData)
{
    if (!pointsInto(oldRoot, oldData))
        return {};

    auto *newCb = allocateCloneableControlBlock<U, M>(allocator, *oldRoot);
    const auto offset = static_cast<const std::byte *>(oldData)
                        - reinterpret_cast<const std::byte *>(oldRoot);
    auto *rawNewRoot = const_cast<std::remove_cv_t<U> *>(newCb->data.get());
    return {newCb, reinterpret_cast<std::byte *>(rawNewRoot) + offset};
}

// The clone a CopyOnWritePtr falls back to when its block cannot clone itself:
// copies the U at root, U being the static type of the pointer it was made from
template<typename U, SharedMutex M>
CloneResult<M> cloneStatic(const void *root, const void *oldData)
{
    return cloneObject<U, M>(std::allocator<std::byte>{}, static_cast<const U *>(root), oldData);
}

// Cloneable is only set for the blocks made by makeCopyOnWrite(),
// allocateCopyOnWrite() and a detaching CopyOnWritePtr. Such a block knows the
// complete type and the allocator of its object, so it clones without slicing.
template<typename T, SharedMutex M, typename Allocator, bool Cloneable>
struct InplaceControlBlock final
    : AllocatedControlBlock<InplaceControlBlock<T, M, Allocator, Cloneable>, M, Allocator>
{
    using Base =
        AllocatedControlBlock<InplaceControlBlock<T, M, Allocator, Cloneable>, M, Allocator>;
    using typename Base::BlockPointer;

    ObjectStorage<T> data;

    template<typename... Args>
    explicit InplaceControlBlock(const Allocator &sourceAllocator,
                                 BlockPointer sourceAllocation,
                                 Args &&...args)
        : Base(sourceAllocator, sourceAllocation)
    {
        data.construct(this->allocator, std::forward<Args>(args)...);
    }

    void destroyData() noexcept override { data.destroy(this->allocator); }

    CloneResult<M> clone([[maybe_unused]] const void *oldData) const override
    {
        if constexpr (Cloneable)
            return cloneObject<T, M>(this->allocator, data.get(), oldData);
        else
            return {};
    }
};

template<typename Block, typename Allocator, typename... Args>
Block *allocateControlBlock(const Allocator &allocator, Args &&...args)
{
    using BlockAllocator = typename Block::BlockAllocator;
    using BlockTraits = typename Block::BlockTraits;
    using BlockPointer = typename Block::BlockPointer;

    BlockAllocator blockAllocator(allocator);
    BlockPointer allocation = BlockTraits::allocate(blockAllocator, 1);
    struct DeallocationGuard
    {
        BlockAllocator &allocator;
        BlockPointer allocation;
        bool active{true};

        ~DeallocationGuard()
        {
            if (active)
                BlockTraits::deallocate(allocator, allocation, 1);
        }
    } guard{blockAllocator, allocation};

    Block *block = std::to_address(allocation);
    BlockTraits::construct(blockAllocator,
                           block,
                           allocator,
                           allocation,
                           std::forward<Args>(args)...);
    guard.active = false;
    return block;
}

template<typename T, SharedMutex M, typename Allocator, typename... Args>
InplaceControlBlock<T, M, Allocator> *
allocateInplaceControlBlock(const Allocator &allocator, Args &&...args)
{
    using Block = InplaceControlBlock<T, M, Allocator>;
    auto *cb = allocateControlBlock<Block>(allocator, std::forward<Args>(args)...);
    armSharedFromThis(cb, cb->data.get());
    return cb;
}

template<typename T, SharedMutex M, typename Allocator, typename... Args>
InplaceControlBlock<T, M, Allocator, true> *
allocateCloneableControlBlock(const Allocator &allocator, Args &&...args)
{
    using Block = InplaceControlBlock<T, M, Allocator, true>;
    auto *cb = allocateControlBlock<Block>(allocator, std::forward<Args>(args)...);
    armSharedFromThis(cb, cb->data.get());
    return cb;
}

template<typename Y, SharedMutex M, typename Deleter, typename Allocator>
struct ExternalControlBlock final
    : AllocatedControlBlock<ExternalControlBlock<Y, M, Deleter, Allocator>, M, Allocator>
{
    using Base =
        AllocatedControlBlock<ExternalControlBlock<Y, M, Deleter, Allocator>, M, Allocator>;
    using typename Base::BlockPointer;

    Y *pointer;
    [[no_unique_address]] Deleter deleter;

    ExternalControlBlock(const Allocator &sourceAllocator,
                         BlockPointer sourceAllocation,
                         Y *sourcePointer,
                         const Deleter &sourceDeleter)
        : Base(sourceAllocator, sourceAllocation)
        , pointer(sourcePointer)
        , deleter(sourceDeleter)
    {}

    void destroyData() noexcept override
    {
        deleter(pointer);
        pointer = nullptr;
    }

    void *getDeleter(const std::type_info &type) noexcept override
    {
        return type == typeid(Deleter) ? std::addressof(deleter) : nullptr;
    }
};

template<typename Y, SharedMutex M, typename Deleter, typename Allocator>
ExternalControlBlock<Y, M, Deleter, Allocator> *
allocateExternalControlBlock(const Allocator &allocator, Y *pointer, const Deleter &deleter)
{
    using Block = ExternalControlBlock<Y, M, Deleter, Allocator>;
    auto *cb = allocateControlBlock<Block>(allocator, pointer, deleter);
    armSharedFromThis(cb, pointer);
    return cb;
}

// What a SharedCopyOnWritePtr and all of its copies point to, and the
// CopyOnWritePtr they were made from: the one place that knows which block holds
// the value right now. A write that finds the block shared with somebody outside
// of the slot clones it and moves the slot over, and with it every pointer
// sharing the slot.
//
// The object is still protected by the mutex of its block, like everywhere else.
// The spin lock only makes "which block is current" and "take a reference to it"
// one step. It is held for a few instructions, never while waiting for a block's
// mutex and never while user code runs. Cloning is the one thing that takes
// long, so the writers wanting to do it queue on a mutex of its own.
//
// "Shared with somebody outside" is sharedRefs > 1 + pins: one reference is the
// slot's own, and the pins are the references the slot handed to its own lock
// guards and to the threads still waiting for the lock. Those are not other
// owners. Counting them as such would clone the object on every contended write.
template<SharedMutex M>
struct CopyOnWriteSlot
{
    using CloneFunction = CloneResult<M> (*)(const void *, const void *);

    // The block the slot pointed to at one point in time, and a reference to it
    struct Pinned
    {
        ControlBlockBase<M> *cb;
        void *data;
        CloneFunction clone;
        const void *cloneRoot;
        bool shared; // owned outside of the slot too, as far as it was known then
    };

    // The pointers sharing the slot, the CopyOnWritePtr it belongs to included
    std::atomic<std::size_t> refs{1};
    // The WeakCopyOnWritePtrs watching the slot, plus one held collectively by the
    // pointers and the pins. The object goes with the last of those, the slot
    // itself with the last observer.
    std::atomic<std::size_t> weakRefs{1};

    // Held by the one writer that clones the object, so writers racing for the
    // first write do not each make a clone. Never taken on any other path.
    M detachMutex;

    // Guards everything below
    SpinLock spin;
    bool orphaned{false}; // the last pointer is gone, the last pin retires the slot
    ControlBlockBase<M> *cb; // never null, the slot owns one reference
    void *data;
    std::size_t pins{0}; // references to cb handed out by pin()
    // All pins that are still out, those to earlier blocks included. A lock guard
    // can outlive every pointer, and it needs the slot to return its pin to.
    std::size_t livePins{0};
    // How to clone an object adopted from a plain SharedPtr, see CopyOnWritePtr.
    // Only ever describes the block the slot was created with.
    CloneFunction clone;
    const void *cloneRoot;

    // Not guarded by the spin lock: where the CopyOnWritePtr the slot was made
    // from looks at the object, counted like the offset of a
    // SharedCopyOnWritePtr. At most one such pointer is attached to a slot (its
    // copies are independent, a move takes the slot along) and it alone changes
    // this, in operations that need exclusive access to it anyway.
    std::ptrdiff_t elementOffset{0};

    CopyOnWriteSlot(ControlBlockBase<M> *sourceCb,
                    void *sourceData,
                    CloneFunction sourceClone,
                    const void *sourceCloneRoot) noexcept
        : cb(sourceCb)
        , data(sourceData)
        , clone(sourceClone)
        , cloneRoot(sourceCloneRoot)
    {}

    virtual ~CopyOnWriteSlot() = default;
    virtual void destroySelf() noexcept = 0;

    void addRef() noexcept { refs.fetch_add(1, std::memory_order_relaxed); }

    // One more pointer for WeakCopyOnWritePtr::lock(), unless the last one is gone
    // already: a slot that lost its last pointer never gets one again
    bool tryAddRef() noexcept
    {
        std::size_t count = refs.load(std::memory_order_relaxed);
        while (count != 0) {
            if (refs.compare_exchange_weak(count,
                                           count + 1,
                                           std::memory_order_acquire,
                                           std::memory_order_relaxed)) {
                return true;
            }
        }
        return false;
    }

    void release() noexcept
    {
        if (refs.fetch_sub(1, std::memory_order_acq_rel) != 1)
            return;

        // No pointer is left to pin anything, but a guard may still be out
        bool last = false;
        {
            const std::lock_guard guard(spin);
            orphaned = true;
            last = livePins == 0;
        }
        if (last)
            retire();
    }

    // The last pointer and the last pin are gone: the object goes, the slot stays
    // for the observers that still watch it
    void retire() noexcept
    {
        cb->releaseShared();
        releaseWeak();
    }

    void addWeak() noexcept { weakRefs.fetch_add(1, std::memory_order_relaxed); }

    void releaseWeak() noexcept
    {
        if (weakRefs.fetch_sub(1, std::memory_order_acq_rel) == 1)
            destroySelf();
    }

    Pinned pin() noexcept
    {
        const std::lock_guard guard(spin);
        const bool shared = cb->sharedRefs.load(std::memory_order_acquire) > pins + 1;
        cb->addShared();
        ++pins;
        ++livePins;
        return {cb, data, clone, cloneRoot, shared};
    }

    void unpin(ControlBlockBase<M> *block) noexcept
    {
        bool current = false;
        bool last = false;
        {
            const std::lock_guard guard(spin);
            current = block == cb;
            if (current) {
                // Both counts drop in one step, or a writer looking in between
                // would take this reference for an outside owner. The slot's own
                // reference keeps the block alive, nothing is destroyed in here.
                --pins;
                block->releaseShared();
            }
            --livePins;
            last = orphaned && livePins == 0;
        }
        // Unless it is ours to retire, the slot may be gone from here on

        // The slot had moved on, this was a plain reference to an earlier block
        if (!current)
            block->releaseShared();
        if (last)
            retire();
    }

    // A reference to the current block for a pointer that goes its own way. Not a
    // pin: from here on the block is shared with somebody outside of the slot.
    Pinned share() noexcept
    {
        const std::lock_guard guard(spin);
        cb->addShared();
        return {cb, data, clone, cloneRoot, true};
    }

    const void *currentOwner() noexcept
    {
        const std::lock_guard guard(spin);
        return cb;
    }

    std::size_t currentUseCount() noexcept
    {
        const std::lock_guard guard(spin);
        return cb->sharedRefs.load(std::memory_order_relaxed);
    }

    bool isCurrent(const ControlBlockBase<M> *block) noexcept
    {
        const std::lock_guard guard(spin);
        return block == cb;
    }

    enum class CastCheck { Allowed, Refused, Moved };

    // Whether a pointer of the slot that looks at a From may get a sibling
    // looking at a To, given that the cast worked on the object in block.
    //
    // A block that clones itself copies its complete type, so whatever a cast
    // finds in the object is in every clone too. An object adopted from a plain
    // SharedPtr is different until it has been cloned once: it is cloned as the
    // type it was adopted as, and a pointer to anything beyond that type would
    // be left pointing past its object. Only what is known to be inside is let
    // through: a base of From, or the adopted type itself.
    template<typename From, typename To>
    CastCheck admitCast(const ControlBlockBase<M> *block) noexcept
    {
        const std::lock_guard guard(spin);
        if (block != cb)
            return CastCheck::Moved;
        if (!clone)
            return CastCheck::Allowed;

        if constexpr (std::is_convertible_v<std::remove_cv_t<From> *, std::remove_cv_t<To> *>) {
            return CastCheck::Allowed;
        } else if constexpr (std::copy_constructible<std::remove_cv_t<To>>) {
            return clone == &cloneStatic<std::remove_cv_t<To>, M> ? CastCheck::Allowed
                                                                  : CastCheck::Refused;
        } else {
            return CastCheck::Refused;
        }
    }

    std::size_t currentWeakCount() noexcept
    {
        const std::lock_guard guard(spin);
        return cb->weakRefs.load(std::memory_order_relaxed);
    }

    bool sharesWithOthers(const ControlBlockBase<M> *block) noexcept
    {
        const std::lock_guard guard(spin);
        return block == cb && cb->sharedRefs.load(std::memory_order_acquire) > pins + 1;
    }

    bool ownsExclusively(const ControlBlockBase<M> *block) noexcept
    {
        const std::lock_guard guard(spin);
        return block == cb && cb->sharedRefs.load(std::memory_order_acquire) <= pins + 1;
    }

    // Unpins when a lock or a clone throws
    struct PinGuard
    {
        CopyOnWriteSlot *slot;
        ControlBlockBase<M> *cb;
        bool active{true};

        ~PinGuard()
        {
            if (active)
                slot->unpin(cb);
        }
    };

    // Both return the block pinned and locked, for a lock guard to adopt

    Pinned lockShared()
    {
        const Pinned pinned = pin();
        PinGuard guard{this, pinned.cb};
        // A writer may move the slot on while we wait. Reading the block that was
        // current when the read began is still a correct read, it only came first.
        pinned.cb->mutex.lock_shared();
        guard.active = false;
        return pinned;
    }

    Pinned lockUnique()
    {
        for (;;) {
            const Pinned pinned = pin();
            PinGuard guard{this, pinned.cb};

            if (pinned.shared) {
                detach(pinned);
                continue; // lock whatever the slot points to now
            }

            pinned.cb->mutex.lock();
            // Waiting for the lock can take a while, so look again now that
            // nobody can get in: a snapshot taken in the meantime must not see
            // this write, and another writer may have moved the slot on.
            if (ownsExclusively(pinned.cb)) {
                guard.active = false;
                return pinned;
            }
            pinned.cb->mutex.unlock();
        }
    }

    // Moves the slot to a copy of the block it shares with others
    void detach(const Pinned &pinned)
    {
        const std::lock_guard serialize(detachMutex);

        ControlBlockBase<M> *previous = nullptr;
        {
            // Protects the copy constructor. Shared, the block is not ours alone.
            // Kept until the slot has moved: the other owner may be gone by then,
            // and a writer that finds the block unshared would write to it in
            // place, behind the back of a clone that is about to replace it.
            std::shared_lock lock(pinned.cb->mutex);

            // Another writer has moved the slot already, or the other owners are
            // gone: nothing to clone, lock whatever the slot points to now
            if (!sharesWithOthers(pinned.cb))
                return;

            CloneResult<M> cloned = pinned.cb->clone(pinned.data);
            if (!cloned.cb && pinned.clone)
                cloned = pinned.clone(pinned.cloneRoot, pinned.data);

            // See CopyOnWritePtr::detach(), which refuses the same way
            if (!cloned.cb) {
                throw std::logic_error(
                    "safe::SharedCopyOnWritePtr cannot clone the object it shares");
            }

            // Only the holder of detachMutex moves the slot, it is still where
            // sharesWithOthers() saw it
            const std::lock_guard guard(spin);
            previous = cb;
            cb = cloned.cb;
            data = cloned.data;
            // The pins belong to the previous block, where they are ordinary
            // references now
            pins = 0;
            // The new block clones itself
            clone = nullptr;
            cloneRoot = nullptr;
        }
        previous->releaseShared();
    }
};

template<SharedMutex M, typename Allocator>
struct AllocatedCopyOnWriteSlot final : CopyOnWriteSlot<M>
{
    using Base = CopyOnWriteSlot<M>;
    using BlockAllocator =
        typename std::allocator_traits<Allocator>::template rebind_alloc<AllocatedCopyOnWriteSlot>;
    using BlockTraits = std::allocator_traits<BlockAllocator>;
    using BlockPointer = typename BlockTraits::pointer;

    [[no_unique_address]] Allocator allocator;
    BlockPointer allocation;

    AllocatedCopyOnWriteSlot(const Allocator &sourceAllocator,
                             BlockPointer sourceAllocation,
                             ControlBlockBase<M> *sourceCb,
                             void *sourceData,
                             typename Base::CloneFunction sourceClone,
                             const void *sourceCloneRoot)
        : Base(sourceCb, sourceData, sourceClone, sourceCloneRoot)
        , allocator(sourceAllocator)
        , allocation(sourceAllocation)
    {}

    void destroySelf() noexcept override
    {
        BlockAllocator blockAllocator(allocator);
        BlockPointer blockAllocation = allocation;
        BlockTraits::destroy(blockAllocator, this);
        BlockTraits::deallocate(blockAllocator, blockAllocation, 1);
    }
};

// How far the To base class is from the From a pointer of the slot looks at.
// Asked of the live object, a virtual base is wherever its complete type put it.
template<typename From, typename To, SharedMutex M>
std::ptrdiff_t baseOffset(CopyOnWriteSlot<M> *slot, std::ptrdiff_t fromOffset) noexcept
{
    const auto pinned = slot->pin();
    From *from = reinterpret_cast<From *>(static_cast<std::byte *>(pinned.data) + fromOffset);
    To *to = from;
    const auto offset = static_cast<const std::byte *>(static_cast<const void *>(to))
                        - static_cast<const std::byte *>(static_cast<const void *>(from));
    slot->unpin(pinned.cb);
    return offset;
}

// Drops the reference a lock guard holds on its block. A guard created through a
// slot has to tell the slot, which keeps count of them.
template<SharedMutex M>
void releaseGuarded(ControlBlockBase<M> *cb, CopyOnWriteSlot<M> *slot) noexcept
{
    if (slot)
        slot->unpin(cb);
    else
        cb->releaseShared();
}
} // namespace detail

// Both guards keep the control block alive for as long as they exist, so a guard
// stays valid even if the pointer it came from is reassigned or destroyed while
// the lock is held. They can only be created by the pointer classes.
template<typename T, SharedMutex M>
class [[nodiscard]] SharedLockGuard
{
    template<typename, SharedMutex>
    friend class SharedPtr;

    template<typename, SharedMutex>
    friend class CopyOnWritePtr;

    template<typename, SharedMutex>
    friend class SharedCopyOnWritePtr;

    const T *m_data;
    detail::ControlBlockBase<M> *m_cb;
    // Only set by a pointer that shares a slot, see detail::releaseGuarded()
    detail::CopyOnWriteSlot<M> *m_slot{nullptr};

    SharedLockGuard(detail::ControlBlockBase<M> *cb, const T *data)
        : m_data(data)
        , m_cb(cb)
    {
        // Lock first: if it throws the destructor never runs, so the reference
        // must not be taken yet. The creating pointer keeps the block alive here.
        m_cb->mutex.lock_shared();
        m_cb->addShared();
    }

    // Adopts a block the slot has already pinned and locked. The pin keeps the
    // slot alive too.
    SharedLockGuard(detail::CopyOnWriteSlot<M> *slot,
                    detail::ControlBlockBase<M> *cb,
                    const T *data) noexcept
        : m_data(data)
        , m_cb(cb)
        , m_slot(slot)
    {}

public:
    ~SharedLockGuard()
    {
        // Unlock before dropping the reference: releasing the last one destroys
        // the control block, and with it the mutex
        if (m_cb) {
            m_cb->mutex.unlock_shared();
            detail::releaseGuarded(m_cb, m_slot);
        }
    }

    SharedLockGuard(SharedLockGuard &&other) noexcept
        : m_data(std::exchange(other.m_data, nullptr))
        , m_cb(std::exchange(other.m_cb, nullptr))
        , m_slot(std::exchange(other.m_slot, nullptr))
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

    template<typename, SharedMutex>
    friend class CopyOnWritePtr;

    template<typename, SharedMutex>
    friend class SharedCopyOnWritePtr;

    T *m_data;
    detail::ControlBlockBase<M> *m_cb;
    // Only set by a pointer that shares a slot, see detail::releaseGuarded()
    detail::CopyOnWriteSlot<M> *m_slot{nullptr};

    UniqueLockGuard(detail::ControlBlockBase<M> *cb, T *data)
        : m_data(data)
        , m_cb(cb)
    {
        // Lock first: if it throws the destructor never runs, so the reference
        // must not be taken yet. The creating pointer keeps the block alive here.
        m_cb->mutex.lock();
        m_cb->addShared();
    }

    // Adopts a block the slot has already pinned and locked. The pin keeps the
    // slot alive too.
    UniqueLockGuard(detail::CopyOnWriteSlot<M> *slot,
                    detail::ControlBlockBase<M> *cb,
                    T *data) noexcept
        : m_data(data)
        , m_cb(cb)
        , m_slot(slot)
    {}

public:
    ~UniqueLockGuard()
    {
        // Unlock before dropping the reference: releasing the last one destroys
        // the control block, and with it the mutex
        if (m_cb) {
            m_cb->mutex.unlock();
            detail::releaseGuarded(m_cb, m_slot);
        }
    }

    UniqueLockGuard(UniqueLockGuard &&other) noexcept
        : m_data(std::exchange(other.m_data, nullptr))
        , m_cb(std::exchange(other.m_cb, nullptr))
        , m_slot(std::exchange(other.m_slot, nullptr))
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

    template<typename, SharedMutex>
    friend class SharedCopyOnWritePtr;

    template<typename U, SharedMutex Mx, typename... Args>
    friend SharedPtr<U, Mx> makeShared(Args &&...args);

    template<typename U, SharedMutex Mx, typename Allocator, typename... Args>
    friend SharedPtr<U, Mx> allocateShared(const Allocator &allocator, Args &&...args);

    template<typename Deleter, typename U, SharedMutex Mx>
    friend constexpr Deleter *getDeleter(const SharedPtr<U, Mx> &pointer) noexcept;

    template<typename U, typename V, SharedMutex Mx>
    friend constexpr SharedPtr<U, Mx> staticCast(const SharedPtr<V, Mx> &other) noexcept;

    template<typename U, typename V, SharedMutex Mx>
    friend constexpr SharedPtr<U, Mx> dynamicCast(const SharedPtr<V, Mx> &other) noexcept;

    template<typename U, typename V, SharedMutex Mx>
    friend constexpr SharedPtr<U, Mx> constCast(const SharedPtr<V, Mx> &other) noexcept;

    template<typename...>
    friend class ScopedLock;

    // Private, not protected: a protected control block would let any derived class
    // forge a pointer or drop a reference. The friends above are the only holders.
private:
    detail::ControlBlockBase<M> *m_cb{nullptr};
    T *m_data{nullptr};

    // Adopts an existing reference, it does not take one. Only the factories,
    // casts, WeakPtr::lock() and CopyOnWritePtr can hand out a control block.
    constexpr SharedPtr(detail::ControlBlockBase<M> *cb, T *data) noexcept
        : m_cb(cb)
        , m_data(data)
    {}

    constexpr void release() noexcept
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

    constexpr SharedPtr() = default;

    constexpr SharedPtr(std::nullptr_t) noexcept {}

    template<typename Y>
        requires std::is_convertible_v<Y *, T *>
    explicit SharedPtr(Y *pointer)
        : SharedPtr(pointer, detail::DefaultDelete<Y>{})
    {}

    template<typename Y, typename Deleter>
        requires(std::is_convertible_v<Y *, T *>
                 && std::copy_constructible<std::decay_t<Deleter>>
                 && std::is_nothrow_invocable_v<std::decay_t<Deleter> &, Y *>)
    SharedPtr(Y *pointer, Deleter deleter)
        : SharedPtr(pointer, std::move(deleter), std::allocator<std::byte>{})
    {}

    template<typename Y, typename Deleter, typename Allocator>
        requires(std::is_convertible_v<Y *, T *>
                 && std::copy_constructible<std::decay_t<Deleter>>
                 && std::copy_constructible<std::decay_t<Allocator>>
                 && std::is_nothrow_invocable_v<std::decay_t<Deleter> &, Y *>)
    SharedPtr(Y *pointer, Deleter deleter, Allocator allocator)
    {
        using StoredDeleter = std::decay_t<Deleter>;
        using StoredAllocator = std::decay_t<Allocator>;

        SAFECPP_EXPECT(pointer != nullptr, "SharedPtr cannot own a null pointer!");

        struct PointerGuard
        {
            Y *pointer;
            StoredDeleter &deleter;
            bool active{true};

            ~PointerGuard()
            {
                if (active)
                    deleter(pointer);
            }
        } guard{pointer, deleter};

        auto *cb = detail::allocateExternalControlBlock<Y, M, StoredDeleter, StoredAllocator>(
            allocator, pointer, deleter);
        m_cb = cb;
        m_data = static_cast<T *>(pointer);
        guard.active = false;
    }

    constexpr SharedPtr(const SharedPtr &other) noexcept
        : m_cb(other.m_cb)
        , m_data(other.m_data)
    {
        if (m_cb)
            m_cb->addShared();
    }

    constexpr SharedPtr(SharedPtr &&other) noexcept
        : m_cb(std::exchange(other.m_cb, nullptr))
        , m_data(std::exchange(other.m_data, nullptr))
    {}

    // Shares ownership with owner while exposing an address protected by the
    // owner's mutex. The caller must keep alias within the owner's lifetime.
    // Both are checked before anything is taken, so a throw leaves owner as it was.
    template<typename U>
    SharedPtr(const SharedPtr<U, M> &owner, T *alias) noexcept(!SAFECPP_THROW_ON_NULL)
        : m_cb(owner.m_cb)
        , m_data(alias)
    {
        SAFECPP_EXPECT(m_cb != nullptr, "SharedPtr alias owner is null!");
        SAFECPP_EXPECT(m_data != nullptr, "SharedPtr alias is null!");
        m_cb->addShared();
    }

    template<typename U>
    SharedPtr(SharedPtr<U, M> &&owner, T *alias) noexcept(!SAFECPP_THROW_ON_NULL)
    {
        SAFECPP_EXPECT(owner.m_cb != nullptr, "SharedPtr alias owner is null!");
        SAFECPP_EXPECT(alias != nullptr, "SharedPtr alias is null!");
        m_cb = std::exchange(owner.m_cb, nullptr);
        m_data = alias;
        owner.m_data = nullptr;
    }

    // Implicit conversion to a compatible pointer type (e.g. Derived -> Base)
    template<typename U>
        requires(!std::is_same_v<U, T> && std::is_convertible_v<U *, T *>)
    constexpr SharedPtr(const SharedPtr<U, M> &other) noexcept
        : m_cb(other.m_cb)
        , m_data(static_cast<T *>(other.m_data))
    {
        if (m_cb)
            m_cb->addShared();
    }

    template<typename U>
        requires(!std::is_same_v<U, T> && std::is_convertible_v<U *, T *>)
    constexpr SharedPtr(SharedPtr<U, M> &&other) noexcept
        : m_cb(std::exchange(other.m_cb, nullptr))
        , m_data(static_cast<T *>(std::exchange(other.m_data, nullptr)))
    {}

    // A plain shared view of the object a CopyOnWritePtr has right now: the one
    // its slot holds, once it shares a slot with SharedCopyOnWritePtrs. Implicit,
    // so a CopyOnWritePtr can be passed where a const SharedPtr & is expected. A
    // write through the view goes to that object in place, for every owner, and
    // never clones. The view stays with the object, not with the pointer.
    template<typename U>
        requires std::is_convertible_v<U *, T *>
    constexpr SharedPtr(const CopyOnWritePtr<U, M> &other) noexcept
    {
        if (auto *slot = other.slot()) {
            const auto shared = slot->share();
            m_cb = shared.cb;
            m_data = other.resolve(slot, shared.data);
        } else {
            m_cb = other.m_pointer.m_cb;
            m_data = other.m_pointer.m_data;
            if (m_cb)
                m_cb->addShared();
        }
    }

    template<typename U>
        requires std::is_convertible_v<U *, T *>
    constexpr SharedPtr(CopyOnWritePtr<U, M> &&other) noexcept
        : SharedPtr(static_cast<const CopyOnWritePtr<U, M> &>(other))
    {
        other.reset();
    }

    template<typename U>
    SharedPtr(const CopyOnWritePtr<U, M> &owner, T *alias) noexcept(!SAFECPP_THROW_ON_NULL)
        : SharedPtr(SharedPtr<U, M>(owner), alias)
    {}

    constexpr ~SharedPtr() { release(); }

    constexpr SharedPtr &operator=(const SharedPtr &other) noexcept
    {
        if (this != &other)
            SharedPtr(other).swap(*this);
        return *this;
    }

    constexpr SharedPtr &operator=(SharedPtr &&other) noexcept
    {
        if (this != &other)
            SharedPtr(std::move(other)).swap(*this);
        return *this;
    }

    template<typename U>
        requires(!std::is_same_v<U, T> && std::is_convertible_v<U *, T *>)
    constexpr SharedPtr &operator=(const SharedPtr<U, M> &other) noexcept
    {
        SharedPtr(other).swap(*this);
        return *this;
    }

    template<typename U>
        requires(!std::is_same_v<U, T> && std::is_convertible_v<U *, T *>)
    constexpr SharedPtr &operator=(SharedPtr<U, M> &&other) noexcept
    {
        SharedPtr(std::move(other)).swap(*this);
        return *this;
    }

    template<typename U>
        requires std::is_convertible_v<U *, T *>
    constexpr SharedPtr &operator=(const CopyOnWritePtr<U, M> &other) noexcept
    {
        SharedPtr(other).swap(*this);
        return *this;
    }

    template<typename U>
        requires std::is_convertible_v<U *, T *>
    constexpr SharedPtr &operator=(CopyOnWritePtr<U, M> &&other) noexcept
    {
        SharedPtr(std::move(other)).swap(*this);
        return *this;
    }

    constexpr SharedPtr &operator=(std::nullptr_t) noexcept
    {
        release();
        return *this;
    }

    constexpr void reset() noexcept { release(); }

    template<typename Y>
        requires std::is_convertible_v<Y *, T *>
    void reset(Y *pointer)
    {
        SharedPtr(pointer).swap(*this);
    }

    template<typename Y, typename Deleter>
        requires(std::is_convertible_v<Y *, T *>
                 && std::copy_constructible<std::decay_t<Deleter>>
                 && std::is_nothrow_invocable_v<std::decay_t<Deleter> &, Y *>)
    void reset(Y *pointer, Deleter deleter)
    {
        SharedPtr(pointer, std::move(deleter)).swap(*this);
    }

    template<typename Y, typename Deleter, typename Allocator>
        requires(std::is_convertible_v<Y *, T *>
                 && std::copy_constructible<std::decay_t<Deleter>>
                 && std::copy_constructible<std::decay_t<Allocator>>
                 && std::is_nothrow_invocable_v<std::decay_t<Deleter> &, Y *>)
    void reset(Y *pointer, Deleter deleter, Allocator allocator)
    {
        SharedPtr(pointer, std::move(deleter), std::move(allocator)).swap(*this);
    }

    constexpr void swap(SharedPtr &other) noexcept
    {
        std::swap(m_cb, other.m_cb);
        std::swap(m_data, other.m_data);
    }

    // Ownership comparison, like std::shared_ptr::owner_equal. Two pointers to the
    // same object through different base classes still compare equal.
    template<typename U>
    constexpr bool operator==(const SharedPtr<U, M> &other) const noexcept
    {
        return m_cb == other.m_cb;
    }

    template<typename U>
    constexpr std::strong_ordering operator<=>(const SharedPtr<U, M> &other) const noexcept
    {
        return std::compare_three_way{}(m_cb, other.m_cb);
    }

    constexpr bool operator==(std::nullptr_t) const noexcept { return m_cb == nullptr; }

    // Identity of the owned object, for comparing pointers. Deliberately opaque:
    // handing out the control block would let callers corrupt the reference counts.
    constexpr const void *owner() const noexcept { return m_cb; }

    // Number of shared owners, including any live lock guard
    constexpr std::size_t useCount() const noexcept
    {
        return m_cb ? m_cb->sharedRefs.load(std::memory_order_relaxed) : 0;
    }

    // Number of observers, plus one collectively held by the shared owners
    constexpr std::size_t weakCount() const noexcept
    {
        return m_cb ? m_cb->weakRefs.load(std::memory_order_relaxed) : 0;
    }

    // Explicit Bool Operator to check if valid
    constexpr explicit operator bool() const noexcept { return m_cb != nullptr; }

    // A null pointer is a precondition violation, see SAFECPP_THROW_ON_NULL. When it
    // does not throw, std::unreachable() states it for release builds as well: it
    // keeps the optimizer from following a path where the pointer is null, and
    // warning about the write it finds there.
    UniqueLockGuard<T, M> writeLock() const
    {
        SAFECPP_EXPECT(m_cb != nullptr, "SharedPtr is null!");
        if (!m_cb)
            std::unreachable();
        return UniqueLockGuard<T, M>(m_cb, m_data);
    }

    SharedLockGuard<T, M> readLock() const
    {
        SAFECPP_EXPECT(m_cb != nullptr, "SharedPtr is null!");
        if (!m_cb)
            std::unreachable();
        return SharedLockGuard<T, M>(m_cb, m_data);
    }
};

// std::static_pointer_cast() equivalent
template<typename T, typename U, SharedMutex M>
[[nodiscard]] constexpr SharedPtr<T, M> staticCast(const SharedPtr<U, M> &other) noexcept
{
    if (!other.m_cb)
        return {};
    other.m_cb->addShared();
    return SharedPtr<T, M>(other.m_cb, static_cast<T *>(other.m_data));
}

// std::dynamic_pointer_cast() equivalent, returns an empty pointer if the cast fails
template<typename T, typename U, SharedMutex M>
[[nodiscard]] constexpr SharedPtr<T, M> dynamicCast(const SharedPtr<U, M> &other) noexcept
{
    if (auto *casted = dynamic_cast<T *>(other.m_data)) {
        if (other.m_cb)
            other.m_cb->addShared();
        return SharedPtr<T, M>(other.m_cb, casted);
    }
    return {};
}

// std::const_pointer_cast() equivalent
template<typename T, typename U, SharedMutex M>
[[nodiscard]] constexpr SharedPtr<T, M> constCast(const SharedPtr<U, M> &other) noexcept
{
    if (!other.m_cb)
        return {};
    other.m_cb->addShared();
    return SharedPtr<T, M>(other.m_cb, const_cast<T *>(other.m_data));
}

template<typename Deleter, typename T, SharedMutex M>
constexpr Deleter *getDeleter(const SharedPtr<T, M> &pointer) noexcept
{
    if (!pointer.m_cb)
        return nullptr;
    return static_cast<Deleter *>(pointer.m_cb->getDeleter(typeid(std::remove_cv_t<Deleter>)));
}

template<typename Deleter, typename T, SharedMutex M>
constexpr Deleter *getDeleter(const CopyOnWritePtr<T, M> &pointer) noexcept
{
    return getDeleter<Deleter>(SharedPtr<T, M>(pointer));
}

template<typename T, SharedMutex M>
constexpr void swap(SharedPtr<T, M> &first, SharedPtr<T, M> &second) noexcept
{
    first.swap(second);
}

template<typename T, SharedMutex M>
constexpr void swap(WeakPtr<T, M> &first, WeakPtr<T, M> &second) noexcept
{
    first.swap(second);
}

template<typename T, SharedMutex M>
class WeakPtr
{
    template<typename, SharedMutex>
    friend class WeakPtr;

    template<typename Object, SharedMutex Mx>
    friend void detail::armSharedFromThis(detail::ControlBlockBase<Mx> *, Object *) noexcept;

private:
    detail::ControlBlockBase<M> *m_cb{nullptr};
    T *m_data{nullptr};

    constexpr WeakPtr(detail::ControlBlockBase<M> *cb, T *data) noexcept
        : m_cb(cb)
        , m_data(data)
    {
        if (m_cb)
            m_cb->addWeak();
    }

    constexpr void release() noexcept
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

    constexpr WeakPtr() = default;

    template<typename U>
        requires std::is_convertible_v<U *, T *>
    constexpr WeakPtr(const SharedPtr<U, M> &shared) noexcept
        : m_cb(shared.m_cb)
        , m_data(static_cast<T *>(shared.m_data))
    {
        if (m_cb)
            m_cb->addWeak();
    }

    // Observes the object a copy-on-write pointer has right now. Like every
    // WeakPtr it follows that object, not the pointer: once the pointer moves on
    // to a clone the observer stays behind, and expires with the last owner of
    // the object it knows.
    template<typename U>
        requires std::is_convertible_v<U *, T *>
    constexpr WeakPtr(const CopyOnWritePtr<U, M> &shared) noexcept
        : WeakPtr(SharedPtr<U, M>(shared))
    {}

    template<typename U>
        requires std::is_convertible_v<U *, T *>
    constexpr WeakPtr(const SharedCopyOnWritePtr<U, M> &shared) noexcept
        : WeakPtr(SharedPtr<U, M>(shared.snapshot()))
    {}

    constexpr WeakPtr(const WeakPtr &other) noexcept
        : m_cb(other.m_cb)
        , m_data(other.m_data)
    {
        if (m_cb)
            m_cb->addWeak();
    }

    // Implicit conversion to a compatible pointer type (e.g. Derived -> Base)
    template<typename U>
        requires(!std::is_same_v<U, T> && std::is_convertible_v<U *, T *>)
    constexpr WeakPtr(const WeakPtr<U, M> &other) noexcept
        : m_cb(other.m_cb)
        , m_data(static_cast<T *>(other.m_data))
    {
        if (m_cb)
            m_cb->addWeak();
    }

    constexpr WeakPtr(WeakPtr &&other) noexcept
        : m_cb(std::exchange(other.m_cb, nullptr))
        , m_data(std::exchange(other.m_data, nullptr))
    {}

    constexpr ~WeakPtr() { release(); }

    constexpr WeakPtr &operator=(const WeakPtr &other) noexcept
    {
        if (this != &other)
            WeakPtr(other).swap(*this);
        return *this;
    }

    constexpr WeakPtr &operator=(WeakPtr &&other) noexcept
    {
        if (this != &other)
            WeakPtr(std::move(other)).swap(*this);
        return *this;
    }

    template<typename U>
        requires(!std::is_same_v<U, T> && std::is_convertible_v<U *, T *>)
    constexpr WeakPtr &operator=(const WeakPtr<U, M> &other) noexcept
    {
        WeakPtr(other).swap(*this);
        return *this;
    }

    template<typename U>
        requires std::is_convertible_v<U *, T *>
    constexpr WeakPtr &operator=(const SharedPtr<U, M> &shared) noexcept
    {
        WeakPtr(shared).swap(*this);
        return *this;
    }

    template<typename U>
        requires std::is_convertible_v<U *, T *>
    constexpr WeakPtr &operator=(const CopyOnWritePtr<U, M> &shared) noexcept
    {
        WeakPtr(shared).swap(*this);
        return *this;
    }

    template<typename U>
        requires std::is_convertible_v<U *, T *>
    constexpr WeakPtr &operator=(const SharedCopyOnWritePtr<U, M> &shared) noexcept
    {
        WeakPtr(shared).swap(*this);
        return *this;
    }

    constexpr void reset() noexcept { release(); }

    constexpr void swap(WeakPtr &other) noexcept
    {
        std::swap(m_cb, other.m_cb);
        std::swap(m_data, other.m_data);
    }

    // Ownership comparison, like SharedPtr. Two observers of the same object
    // compare equal even if they watch it through different base classes, and
    // an expired observer keeps comparing equal to its peers.
    template<typename U>
    constexpr bool operator==(const WeakPtr<U, M> &other) const noexcept
    {
        return m_cb == other.m_cb;
    }

    template<typename U>
    constexpr std::strong_ordering operator<=>(const WeakPtr<U, M> &other) const noexcept
    {
        return std::compare_three_way{}(m_cb, other.m_cb);
    }

    // Identity of the observed object, comparable with SharedPtr::owner().
    // Stays the same once the object is gone, so an expired observer is still
    // findable in a container.
    constexpr const void *owner() const noexcept { return m_cb; }

    constexpr std::size_t useCount() const noexcept
    {
        return m_cb ? m_cb->sharedRefs.load(std::memory_order_relaxed) : 0;
    }

    constexpr bool expired() const noexcept
    {
        return !m_cb || m_cb->sharedRefs.load(std::memory_order_relaxed) == 0;
    }

    constexpr SharedPtr<T, M> lock() const noexcept
    {
        // The data pointer stays valid as long as a shared owner exists, and
        // tryAddShared() only succeeds while T is still alive.
        if (m_cb && m_cb->tryAddShared())
            return SharedPtr<T, M>(m_cb, m_data);
        return SharedPtr<T, M>{};
    }
};

namespace detail {
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
        SAFECPP_EXPECT(((pointers.m_cb != nullptr) && ...), "ScopedLock on a null pointer!");

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
    template<typename Object, SharedMutex Mx>
    friend void detail::armSharedFromThis(detail::ControlBlockBase<Mx> *, Object *) noexcept;

    WeakPtr<T, M> m_weakThis;

protected:
    constexpr EnableSharedFromThis() = default;

    // A copy is a different object, so it must not inherit the back reference.
    // Whoever takes ownership of the copy arms it.
    constexpr EnableSharedFromThis(const EnableSharedFromThis &) noexcept {}
    constexpr EnableSharedFromThis &operator=(const EnableSharedFromThis &) noexcept
    {
        return *this;
    }
    constexpr ~EnableSharedFromThis() = default;

public:
    using ElementType = T;
    using MutexType = M;

    [[nodiscard]] constexpr SharedPtr<T, M> sharedFromThis() const noexcept
    {
        return m_weakThis.lock();
    }
    [[nodiscard]] constexpr WeakPtr<T, M> weakFromThis() const noexcept { return m_weakThis; }
    [[nodiscard]] constexpr SharedPtr<T, M> sharedFromThis() noexcept
    {
        return m_weakThis.lock();
    }
    [[nodiscard]] constexpr WeakPtr<T, M> weakFromThis() noexcept { return m_weakThis; }
};

namespace detail {
template<typename Object, SharedMutex M>
void armSharedFromThis(ControlBlockBase<M> *cb, Object *object) noexcept
{
    if constexpr (!std::is_const_v<Object> && HasSharedFromThis<Object, M>) {
        // EnableSharedFromThis<U, M>, with U deduced from the base rather than
        // from the concrete object type.
        using EsftBase = std::remove_pointer_t<decltype(esftBaseOf<M>(object))>;
        using SharedType = typename EsftBase::ElementType;
        auto *base = static_cast<EsftBase *>(object);
        if (base->m_weakThis.expired())
            base->m_weakThis = WeakPtr<SharedType, M>(cb, static_cast<SharedType *>(object));
    }
}
} // namespace detail

template<typename T, SharedMutex M, typename... Args>
SharedPtr<T, M> makeShared(Args &&...args)
{
    return allocateShared<T, M>(std::allocator<std::byte>{}, std::forward<Args>(args)...);
}

template<typename T, SharedMutex M, typename Allocator, typename... Args>
SharedPtr<T, M> allocateShared(const Allocator &allocator, Args &&...args)
{
    auto *cb =
        detail::allocateInplaceControlBlock<T, M>(allocator, std::forward<Args>(args)...);
    return SharedPtr<T, M>(cb, cb->data.get());
}

// A shared pointer that clones the object the first time it is written to while
// other owners exist.
//
// What gets cloned:
//  - An object created by makeCopyOnWrite() / allocateCopyOnWrite() is cloned as
//    its complete type and with its allocator, no matter which base class the
//    detaching pointer looks at it through.
//  - An object adopted from a plain SharedPtr<U> is cloned as U, the static type
//    of that pointer, into a std::allocator block: the deleter and allocator it
//    was created with say nothing about how to make a second one. Adopt it
//    through a SharedPtr to the complete type, or it is sliced to U. U has to be
//    copy constructible, which is checked when the pointer is adopted.
//
// Interaction with WeakPtr: a WeakPtr observes a *control block*, not a pointer.
// Once this pointer detaches it moves to a fresh block, so observers taken before
// the detach keep watching the original object and never see the clone. Take the
// WeakPtr from the pointer you actually want to follow, or use a plain SharedPtr
// if observers must track every write.
//
// Copies and SharedCopyOnWritePtr: a copy of a CopyOnWritePtr is an independent
// pointer, a write through it clones for the copy alone. A SharedCopyOnWritePtr
// made from the pointer is the opposite, it stays with this pointer. The two
// share a slot from then on, and a write through either reaches both. See there.
//
// It is not a SharedPtr, but converts to one: SharedPtr<T> view = pointer, or the
// pointer passed where a const SharedPtr<T> & is expected. The view shares the
// object the pointer has at that moment and a write through it goes to that
// object in place, for every owner, without cloning: the way to opt out of the
// copy on write. It stays with that object: once the pointer clones on a write of
// its own, the view is left with the owners of the object it had.
//
// There are no casts, see staticCast().
template<typename T, SharedMutex M>
class CopyOnWritePtr
{
    using Slot = detail::CopyOnWriteSlot<M>;
    using CloneFunction = detail::CloneResult<M> (*)(const void *, const void *);

    template<typename, SharedMutex>
    friend class SharedPtr;

    template<typename, SharedMutex>
    friend class CopyOnWritePtr;

    template<typename, SharedMutex>
    friend class SharedCopyOnWritePtr;

    template<typename U, SharedMutex Mx, typename Allocator, typename... Args>
        requires std::copy_constructible<U>
    friend CopyOnWritePtr<U, Mx> allocateCopyOnWrite(const Allocator &allocator, Args &&...args);

    // The object, as long as the pointer shares no slot. A member and not a base
    // class: a SharedPtr reference to it would show this part only, which says
    // nothing once there is a slot. A SharedPtr view is made by converting the
    // pointer instead, and that asks the slot.
    SharedPtr<T, M> m_pointer;

    // How to clone an object adopted from a plain SharedPtr, whose block cannot
    // do it. Captured where the static type is still known and carried along by
    // the conversions to a base class. It describes the object of m_cloneOwner
    // only, and is compared with it before use.
    CloneFunction m_clone{nullptr};
    const void *m_cloneRoot{nullptr};
    const void *m_cloneOwner{nullptr};

    // The slot shared with the SharedCopyOnWritePtrs made from this pointer, set
    // when the first of them is made. From then on the slot says which object
    // the pointer has: a write through any of them may move it to a clone.
    //
    // Making that first SharedCopyOnWritePtr is a const operation, several
    // threads may do it to the same pointer at once. So it changes nothing but
    // this member, and that atomically. In particular the reference in
    // m_pointer stays where it is, although nothing here uses it any more:
    // another thread may be half way through reading it. It is dropped as soon
    // as the pointer is used in a way that needs exclusive access anyway (a
    // move, a write lock). Until then it makes the object look shared, so the
    // first write through the slot clones even if nobody else has the object.
    mutable std::atomic<Slot *> m_slot{nullptr};

    CopyOnWritePtr(detail::ControlBlockBase<M> *cb, T *data) noexcept
        : m_pointer(cb, data)
    {}

    template<typename U>
    constexpr CopyOnWritePtr(const U *root, SharedPtr<U, M> &&other) noexcept
        : m_pointer(std::move(other))
    {
        captureClone(root);
    }

    template<typename U>
    constexpr void captureClone(const U *root) noexcept
    {
        if (root) {
            m_clone = &detail::cloneStatic<std::remove_cv_t<U>, M>;
            m_cloneRoot = root;
            m_cloneOwner = m_pointer.m_cb;
        }
    }

    constexpr Slot *slot() const noexcept
    {
        if consteval {
            return nullptr; // nothing to share in a constant expression
        } else {
            return m_slot.load(std::memory_order_acquire);
        }
    }

    T *resolve(const Slot *slot, void *data) const noexcept
    {
        return reinterpret_cast<T *>(static_cast<std::byte *>(data) + slot->elementOffset);
    }

    // The slot of this pointer and one more reference to it, null for an empty
    // pointer. Creates the slot on first use. Safe to call from several threads
    // at once, see m_slot.
    Slot *attach() const
    {
        Slot *slot = m_slot.load(std::memory_order_acquire);
        if (!slot) {
            if (!m_pointer.m_cb)
                return nullptr;

            using AllocatedSlot = detail::AllocatedCopyOnWriteSlot<M, std::allocator<std::byte>>;
            // See CopyOnWritePtr: what it knows about cloning only counts for the
            // block it was learned from
            const bool knowsClone = m_clone && m_cloneOwner == m_pointer.m_cb;
            SharedPtr<T, M> reference(m_pointer);
            Slot *fresh = detail::allocateControlBlock<AllocatedSlot>(
                std::allocator<std::byte>{},
                reference.m_cb,
                const_cast<std::remove_cv_t<T> *>(reference.m_data),
                knowsClone ? m_clone : nullptr,
                knowsClone ? m_cloneRoot : nullptr);
            // The slot owns that reference now
            reference.m_cb = nullptr;
            reference.m_data = nullptr;

            if (m_slot.compare_exchange_strong(slot,
                                               fresh,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
                slot = fresh;
            } else {
                fresh->release(); // another thread was faster, slot is theirs
            }
        }
        slot->addRef();
        return slot;
    }

    // Only with exclusive access to the pointer. Once it shares a slot, the
    // reference in m_pointer is of no use to anybody: it keeps an object alive
    // that the slot may have left long ago, and makes the slot's object look
    // shared while the slot is still on it.
    void dropOwnReference() noexcept
    {
        if (m_pointer.m_cb) {
            m_pointer.release();
            m_clone = nullptr;
            m_cloneRoot = nullptr;
            m_cloneOwner = nullptr;
        }
    }

    // An independent pointer to the object other has right now
    template<typename U>
    constexpr void copyFrom(const CopyOnWritePtr<U, M> &other) noexcept
    {
        if (Slot *slot = other.slot()) {
            const auto shared = slot->share();
            m_pointer.m_cb = shared.cb;
            m_pointer.m_data = other.resolve(slot, shared.data);
            if (shared.clone) {
                m_clone = shared.clone;
                m_cloneRoot = shared.cloneRoot;
                m_cloneOwner = shared.cb;
            }
            return;
        }

        m_pointer = other.m_pointer;
        m_clone = other.m_clone;
        m_cloneRoot = other.m_cloneRoot;
        m_cloneOwner = other.m_cloneOwner;
    }

    // Takes everything, the slot included: the SharedCopyOnWritePtrs made from
    // other belong to this pointer now
    template<typename U>
    constexpr void moveFrom(CopyOnWritePtr<U, M> &other) noexcept
    {
        if !consteval {
            if (Slot *slot = other.m_slot.exchange(nullptr, std::memory_order_acq_rel)) {
                if constexpr (!std::is_same_v<U, T>)
                    slot->elementOffset += detail::baseOffset<U, T>(slot, slot->elementOffset);
                m_slot.store(slot, std::memory_order_release);
                other.dropOwnReference(); // nobody else is using other, by contract
                return;
            }
        }

        m_pointer = std::move(other.m_pointer);
        m_clone = other.m_clone;
        m_cloneRoot = other.m_cloneRoot;
        m_cloneOwner = other.m_cloneOwner;
    }

    void detach()
    {
        if (!m_pointer.m_cb || m_pointer.m_cb->sharedRefs.load(std::memory_order_acquire) <= 1)
            return;

        detail::CloneResult<M> cloned;
        {
            // Read-Lock old data to protect copy constructor execution. The lock
            // must be released before release(), which may destroy the old block
            // (and with it the mutex) if the other owners vanish meanwhile.
            std::shared_lock lock(m_pointer.m_cb->mutex);
            cloned = m_pointer.m_cb->clone(m_pointer.m_data);
            if (!cloned.cb && m_clone && m_cloneOwner == m_pointer.m_cb)
                cloned = m_clone(m_cloneRoot, m_pointer.m_data);
        }

        // Not reachable through the public interface: every way to make a
        // CopyOnWritePtr leaves it knowing how to clone its object. Writing to the
        // shared object instead would silently break every other owner, so if
        // that ever stops being true, refuse.
        if (!cloned.cb)
            throw std::logic_error("safe::CopyOnWritePtr cannot clone the object it shares");

        m_pointer.release();
        m_pointer.m_cb = cloned.cb;
        m_pointer.m_data = static_cast<T *>(cloned.data);

        // The new block clones itself
        m_clone = nullptr;
        m_cloneRoot = nullptr;
        m_cloneOwner = nullptr;
    }

public:
    using ElementType = T;
    using MutexType = M;

    constexpr CopyOnWritePtr() = default;

    constexpr CopyOnWritePtr(std::nullptr_t) noexcept {}

    // An independent pointer: it shares the object until one of the two writes,
    // and never the slot
    constexpr CopyOnWritePtr(const CopyOnWritePtr &other) noexcept { copyFrom(other); }

    constexpr CopyOnWritePtr(CopyOnWritePtr &&other) noexcept { moveFrom(other); }

    // Up-casts keep the knowledge of how to clone the complete object
    template<typename U>
        requires(!std::is_same_v<U, T> && std::is_convertible_v<U *, T *>)
    constexpr CopyOnWritePtr(const CopyOnWritePtr<U, M> &other) noexcept
    {
        copyFrom(other);
    }

    template<typename U>
        requires(!std::is_same_v<U, T> && std::is_convertible_v<U *, T *>)
    constexpr CopyOnWritePtr(CopyOnWritePtr<U, M> &&other) noexcept
    {
        moveFrom(other);
    }

    // Adopts a shared object. A detach clones it as U, see the class comment.
    template<typename U>
        requires(std::is_convertible_v<U *, T *> && std::copy_constructible<std::remove_cv_t<U>>)
    constexpr CopyOnWritePtr(const SharedPtr<U, M> &other) noexcept
        : m_pointer(other)
    {
        captureClone(other.m_data);
    }

    template<typename U>
        requires(std::is_convertible_v<U *, T *> && std::copy_constructible<std::remove_cv_t<U>>)
    constexpr CopyOnWritePtr(SharedPtr<U, M> &&other) noexcept
        : CopyOnWritePtr(static_cast<const U *>(other.m_data), std::move(other))
    {}

    // An independent pointer to the value a SharedCopyOnWritePtr has right now,
    // the same as its snapshot(). Explicit: the result has left the slot, and a
    // function declared to return a CopyOnWritePtr would otherwise compile and
    // hand out the very copies whose writes get lost.
    template<typename U>
        requires std::is_convertible_v<U *, T *>
    constexpr explicit CopyOnWritePtr(const SharedCopyOnWritePtr<U, M> &other) noexcept
        : CopyOnWritePtr(other.snapshot())
    {}

    constexpr ~CopyOnWritePtr()
    {
        if !consteval {
            if (Slot *slot = m_slot.load(std::memory_order_relaxed))
                slot->release();
        }
    }

    // Assigning replaces the pointer and what it shared. The
    // SharedCopyOnWritePtrs made from it keep their slot and the value it has,
    // they just do not belong to this pointer any more.
    constexpr CopyOnWritePtr &operator=(const CopyOnWritePtr &other) noexcept
    {
        if (this != &other)
            CopyOnWritePtr(other).swap(*this);
        return *this;
    }

    constexpr CopyOnWritePtr &operator=(CopyOnWritePtr &&other) noexcept
    {
        if (this != &other)
            CopyOnWritePtr(std::move(other)).swap(*this);
        return *this;
    }

    // Everything a CopyOnWritePtr can be constructed from can be assigned too
    template<typename Other>
        requires(!std::is_same_v<std::remove_cvref_t<Other>, CopyOnWritePtr>
                 && std::is_convertible_v<Other, CopyOnWritePtr>)
    constexpr CopyOnWritePtr &operator=(Other &&other) noexcept
    {
        CopyOnWritePtr(std::forward<Other>(other)).swap(*this);
        return *this;
    }

    // Hides the SharedPtr overloads that adopt a raw pointer: the block they
    // create cannot be cloned
    constexpr void reset() noexcept { CopyOnWritePtr().swap(*this); }

    constexpr void swap(CopyOnWritePtr &other) noexcept
    {
        m_pointer.swap(other.m_pointer);
        std::swap(m_clone, other.m_clone);
        std::swap(m_cloneRoot, other.m_cloneRoot);
        std::swap(m_cloneOwner, other.m_cloneOwner);
        if !consteval {
            Slot *mine = m_slot.load(std::memory_order_relaxed);
            m_slot.store(other.m_slot.load(std::memory_order_relaxed), std::memory_order_relaxed);
            other.m_slot.store(mine, std::memory_order_relaxed);
        }
    }

    // The members below ask the slot, if there is one: from then on the slot knows
    // the object, m_pointer does not.

    // Ownership comparison, see SharedPtr. Works against a SharedCopyOnWritePtr
    // as well, which defines that one.
    template<typename U>
    constexpr bool operator==(const CopyOnWritePtr<U, M> &other) const noexcept
    {
        return owner() == other.owner();
    }

    template<typename U>
    constexpr bool operator==(const SharedPtr<U, M> &other) const noexcept
    {
        return owner() == other.owner();
    }

    template<typename U>
    constexpr std::strong_ordering operator<=>(const CopyOnWritePtr<U, M> &other) const noexcept
    {
        return std::compare_three_way{}(owner(), other.owner());
    }

    template<typename U>
    constexpr std::strong_ordering operator<=>(const SharedPtr<U, M> &other) const noexcept
    {
        return std::compare_three_way{}(owner(), other.owner());
    }

    constexpr bool operator==(std::nullptr_t) const noexcept { return !bool(*this); }

    constexpr const void *owner() const noexcept
    {
        if (Slot *slot = this->slot())
            return slot->currentOwner();
        return m_pointer.owner();
    }

    // The slot it shares with its SharedCopyOnWritePtrs counts as one owner
    constexpr std::size_t useCount() const noexcept
    {
        if (Slot *slot = this->slot())
            return slot->currentUseCount();
        return m_pointer.useCount();
    }

    constexpr std::size_t weakCount() const noexcept
    {
        if (Slot *slot = this->slot())
            return slot->currentWeakCount();
        return m_pointer.weakCount();
    }

    constexpr explicit operator bool() const noexcept
    {
        return this->slot() != nullptr || m_pointer.m_cb != nullptr;
    }

    UniqueLockGuard<T, M> writeLock()
    {
        if (Slot *slot = this->slot()) {
            // Not const, so nobody else is using this pointer right now
            dropOwnReference();
            const auto pinned = slot->lockUnique();
            return UniqueLockGuard<T, M>(slot, pinned.cb, resolve(slot, pinned.data));
        }

        SAFECPP_EXPECT(m_pointer.m_cb != nullptr, "CopyOnWritePtr is null!");
        detach();
        return m_pointer.writeLock();
    }

    SharedLockGuard<T, M> readLock() const
    {
        if (Slot *slot = this->slot()) {
            const auto pinned = slot->lockShared();
            return SharedLockGuard<T, M>(slot, pinned.cb, resolve(slot, pinned.data));
        }
        SAFECPP_EXPECT(m_pointer.m_cb != nullptr, "CopyOnWritePtr is null!");
        return m_pointer.readLock();
    }
};

template<typename T, SharedMutex M>
constexpr void swap(CopyOnWritePtr<T, M> &first, CopyOnWritePtr<T, M> &second) noexcept
{
    first.swap(second);
}

template<typename T, SharedMutex M, typename... Args>
    requires std::copy_constructible<T>
CopyOnWritePtr<T, M> makeCopyOnWrite(Args &&...args)
{
    return allocateCopyOnWrite<T, M>(std::allocator<std::byte>{}, std::forward<Args>(args)...);
}

template<typename T, SharedMutex M, typename Allocator, typename... Args>
    requires std::copy_constructible<T>
CopyOnWritePtr<T, M> allocateCopyOnWrite(const Allocator &allocator, Args &&...args)
{
    auto *cb =
        detail::allocateCloneableControlBlock<T, M>(allocator, std::forward<Args>(args)...);
    return CopyOnWritePtr<T, M>(cb, cb->data.get());
}

// A CopyOnWritePtr that can be handed out by value: think of it as a
// std::shared_ptr<CopyOnWritePtr<T>> that locks only once.
//
// A copy of a CopyOnWritePtr is an independent pointer, a write through it clones
// the object for the copy alone. That is the wrong thing for a function returning
// an element of a container by value: the write lands in a clone only the
// temporary owns. Make the function return a SharedCopyOnWritePtr instead:
//
//     std::vector<CopyOnWritePtr<Item>> original = ...;
//     std::vector<CopyOnWritePtr<Item>> working = original; // shares every Item
//
//     SharedCopyOnWritePtr<Item> item(std::size_t i) { return working[i]; }
//
//     auto early = item(3);
//     item(3).writeLock()->name = "x"; // working[3] and early have it,
//                                      // original[3] does not
//
// A SharedCopyOnWritePtr made from a CopyOnWritePtr stays with it. The two share
// a slot, and so does every further one made from the same pointer and every
// copy of those. The first write through any of them clones the object once, if
// somebody outside of the slot has it too, and all of them see the result,
// working[3] included. A copy of working[3] is still an independent pointer.
//
//  - Made from a temporary CopyOnWritePtr, or by makeSharedCopyOnWrite() /
//    allocateSharedCopyOnWrite(), the slot belongs to the SharedCopyOnWritePtrs
//    alone.
//  - Locking works like a SharedPtr: the guard gives access to T and holds the
//    one mutex protecting it. A write lock keeps every other pointer of the slot
//    out and waits for their guards to go away, it never clones because of
//    them. The only exception is the write that moves the slot to its clone: it
//    does not wait for the readers of the object it leaves behind, they finish
//    on the value they have.
//  - Making one from a CopyOnWritePtr is a read of that pointer, any number of
//    threads may do it at once. See CopyOnWritePtr::m_slot for what that costs.
//  - snapshot() is the way out: an independent CopyOnWritePtr to the current
//    value, which later writes to the slot do not touch.
//  - The slot is the identity. ==, <=>, owner() and std::hash work on it and do
//    not change when the object is cloned, so unlike a CopyOnWritePtr this pointer
//    can be a key.
//  - The casts stay on the slot: they return a SharedCopyOnWritePtr to another
//    type of the same object, and a write through it clones like any other.
//  - A WeakPtr made from the pointer, weakCount() and == against a SharedPtr or
//    a CopyOnWritePtr are about the object the slot holds at that moment. A
//    WeakCopyOnWritePtr watches the slot instead and locks back into one of these.
//  - Like the other pointers, a pointer object must not be used by two threads at
//    once. Any number of threads may use their own copies, that is the point.
//
// What gets cloned follows the rules of CopyOnWritePtr, including the WeakPtr and
// sharedFromThis() caveat: both follow a block, not the slot.
template<typename T, SharedMutex M>
class SharedCopyOnWritePtr
{
    template<typename, SharedMutex>
    friend class SharedCopyOnWritePtr;

    template<typename, SharedMutex>
    friend class WeakCopyOnWritePtr;

    template<typename U, SharedMutex Mx, typename Allocator, typename... Args>
        requires std::copy_constructible<U>
    friend SharedCopyOnWritePtr<U, Mx> allocateSharedCopyOnWrite(const Allocator &allocator,
                                                                 Args &&...args);

    template<typename V, typename W, SharedMutex Mx>
    friend constexpr SharedCopyOnWritePtr<V, Mx>
    staticCast(const SharedCopyOnWritePtr<W, Mx> &other) noexcept;

    template<typename V, typename W, SharedMutex Mx>
    friend constexpr SharedCopyOnWritePtr<V, Mx>
    dynamicCast(const SharedCopyOnWritePtr<W, Mx> &other) noexcept;

    template<typename V, typename W, SharedMutex Mx>
    friend constexpr SharedCopyOnWritePtr<V, Mx>
    constCast(const SharedCopyOnWritePtr<W, Mx> &other) noexcept;

    using Slot = detail::CopyOnWriteSlot<M>;

    Slot *m_slot{nullptr};
    // Where T lives, in bytes from the address the slot keeps. The slot is shared
    // by pointers to different base classes of the object and moves between
    // blocks, so it cannot remember an address for each of them.
    std::ptrdiff_t m_offset{0};

    // Adopts an existing reference to the slot, it does not take one
    constexpr SharedCopyOnWritePtr(Slot *slot, std::ptrdiff_t offset) noexcept
        : m_slot(slot)
        , m_offset(offset)
    {}

    // Takes source over. If it shares a slot already this pointer joins it,
    // otherwise its object moves into a new one. Leaves source alone if that slot
    // cannot be allocated.
    template<typename U, typename Allocator>
    SharedCopyOnWritePtr(const Allocator &allocator, CopyOnWritePtr<U, M> &&source)
    {
        if (Slot *slot = source.slot()) {
            slot->addRef();
            m_slot = slot;
            m_offset = slot->elementOffset;
            if constexpr (!std::is_same_v<U, T>)
                m_offset += detail::baseOffset<U, T>(slot, m_offset);
            source.reset();
            return;
        }

        SharedPtr<U, M> &reference = source.m_pointer;
        if (!reference.m_cb)
            return;

        using AllocatedSlot = detail::AllocatedCopyOnWriteSlot<M, Allocator>;
        // See CopyOnWritePtr: what it knows about cloning only counts for the
        // block it was learned from
        const bool knowsClone = source.m_clone && source.m_cloneOwner == reference.m_cb;
        T *data = reference.m_data;
        m_slot = detail::allocateControlBlock<AllocatedSlot>(
            allocator,
            reference.m_cb,
            const_cast<std::remove_cv_t<T> *>(data),
            knowsClone ? source.m_clone : nullptr,
            knowsClone ? source.m_cloneRoot : nullptr);

        // The slot owns the reference from here on
        reference.m_cb = nullptr;
        reference.m_data = nullptr;
    }

    constexpr T *resolve(void *data) const noexcept
    {
        return reinterpret_cast<T *>(static_cast<std::byte *>(data) + m_offset);
    }

    // Another pointer of the same slot, looking at whatever cast makes of the T
    // this one looks at. Empty if the cast fails, or must not be allowed.
    template<typename Target, typename Cast>
    constexpr SharedCopyOnWritePtr<Target, M> castTo(Cast cast) const noexcept
    {
        if (!m_slot)
            return {};

        for (;;) {
            const auto pinned = m_slot->pin();
            Target *target = cast(resolve(pinned.data));
            auto check = Slot::CastCheck::Refused;
            std::ptrdiff_t offset = 0;
            if (target) {
                offset = static_cast<const std::byte *>(static_cast<const void *>(target))
                         - static_cast<const std::byte *>(pinned.data);
                check = m_slot->template admitCast<T, Target>(pinned.cb);
            }
            m_slot->unpin(pinned.cb);

            if (check == Slot::CastCheck::Moved)
                continue; // a writer replaced the object meanwhile, look at the new one
            if (check == Slot::CastCheck::Refused)
                return {};

            m_slot->addRef();
            return SharedCopyOnWritePtr<Target, M>(m_slot, offset);
        }
    }

public:
    using ElementType = T;
    using MutexType = M;

    constexpr SharedCopyOnWritePtr() = default;

    constexpr SharedCopyOnWritePtr(std::nullptr_t) noexcept {}

    // Another pointer to the same slot
    constexpr SharedCopyOnWritePtr(const SharedCopyOnWritePtr &other) noexcept
        : m_slot(other.m_slot)
        , m_offset(other.m_offset)
    {
        if (m_slot)
            m_slot->addRef();
    }

    constexpr SharedCopyOnWritePtr(SharedCopyOnWritePtr &&other) noexcept
        : m_slot(std::exchange(other.m_slot, nullptr))
        , m_offset(std::exchange(other.m_offset, 0))
    {}

    // Implicit conversion to a compatible pointer type (e.g. Derived -> Base).
    // Still the same slot, and a write through the base clones the whole object.
    template<typename U>
        requires(!std::is_same_v<U, T> && std::is_convertible_v<U *, T *>)
    constexpr SharedCopyOnWritePtr(const SharedCopyOnWritePtr<U, M> &other) noexcept
        : m_slot(other.m_slot)
        , m_offset(other.m_offset)
    {
        if (m_slot) {
            m_offset += detail::baseOffset<U, T>(m_slot, m_offset);
            m_slot->addRef();
        }
    }

    template<typename U>
        requires(!std::is_same_v<U, T> && std::is_convertible_v<U *, T *>)
    constexpr SharedCopyOnWritePtr(SharedCopyOnWritePtr<U, M> &&other) noexcept
        : m_slot(std::exchange(other.m_slot, nullptr))
        , m_offset(std::exchange(other.m_offset, 0))
    {
        if (m_slot)
            m_offset += detail::baseOffset<U, T>(m_slot, m_offset);
    }

    // Shares the slot of other, which gets one if this is the first
    // SharedCopyOnWritePtr made from it. From here on a write through this
    // pointer is a write through other, see the class comment. Reads other only:
    // any number of threads may do this to the same CopyOnWritePtr at once.
    template<typename U>
        requires std::is_convertible_v<U *, T *>
    SharedCopyOnWritePtr(const CopyOnWritePtr<U, M> &other)
        : m_slot(other.attach())
    {
        if (m_slot) {
            m_offset = m_slot->elementOffset;
            if constexpr (!std::is_same_v<U, T>)
                m_offset += detail::baseOffset<U, T>(m_slot, m_offset);
        }
    }

    // Nothing else can share a temporary: its object moves into a slot that
    // belongs to the SharedCopyOnWritePtrs alone
    template<typename U>
        requires std::is_convertible_v<U *, T *>
    SharedCopyOnWritePtr(CopyOnWritePtr<U, M> &&other)
        : SharedCopyOnWritePtr(std::allocator<std::byte>{}, std::move(other))
    {}

    constexpr ~SharedCopyOnWritePtr()
    {
        if (m_slot)
            m_slot->release();
    }

    // Assigning re-seats this pointer only, the other pointers of the slot it
    // leaves are not affected. Write to the object to change it for all of them.
    constexpr SharedCopyOnWritePtr &operator=(const SharedCopyOnWritePtr &other) noexcept
    {
        if (this != &other)
            SharedCopyOnWritePtr(other).swap(*this);
        return *this;
    }

    constexpr SharedCopyOnWritePtr &operator=(SharedCopyOnWritePtr &&other) noexcept
    {
        if (this != &other)
            SharedCopyOnWritePtr(std::move(other)).swap(*this);
        return *this;
    }

    template<typename U>
        requires(!std::is_same_v<U, T> && std::is_convertible_v<U *, T *>)
    constexpr SharedCopyOnWritePtr &operator=(const SharedCopyOnWritePtr<U, M> &other) noexcept
    {
        SharedCopyOnWritePtr(other).swap(*this);
        return *this;
    }

    template<typename U>
        requires(!std::is_same_v<U, T> && std::is_convertible_v<U *, T *>)
    constexpr SharedCopyOnWritePtr &operator=(SharedCopyOnWritePtr<U, M> &&other) noexcept
    {
        SharedCopyOnWritePtr(std::move(other)).swap(*this);
        return *this;
    }

    constexpr SharedCopyOnWritePtr &operator=(std::nullptr_t) noexcept
    {
        reset();
        return *this;
    }

    constexpr void reset() noexcept { SharedCopyOnWritePtr().swap(*this); }

    constexpr void swap(SharedCopyOnWritePtr &other) noexcept
    {
        std::swap(m_slot, other.m_slot);
        std::swap(m_offset, other.m_offset);
    }

    // Two pointers are equal if they share a slot, no matter which base class
    // they look at the object through
    template<typename U>
    constexpr bool operator==(const SharedCopyOnWritePtr<U, M> &other) const noexcept
    {
        return m_slot == other.m_slot;
    }

    template<typename U>
    constexpr std::strong_ordering
    operator<=>(const SharedCopyOnWritePtr<U, M> &other) const noexcept
    {
        return std::compare_three_way{}(m_slot, other.m_slot);
    }

    constexpr bool operator==(std::nullptr_t) const noexcept { return m_slot == nullptr; }

    // True if the slot holds, right now, the object other owns. Unlike the
    // comparison of two SharedCopyOnWritePtr this stops being true as soon as
    // the slot moves on to a clone. A CopyOnWritePtr that shares the slot always
    // compares equal, it moves along.
    template<typename U>
    constexpr bool operator==(const SharedPtr<U, M> &other) const noexcept
    {
        return (m_slot ? m_slot->currentOwner() : nullptr) == other.owner();
    }

    template<typename U>
    constexpr bool operator==(const CopyOnWritePtr<U, M> &other) const noexcept
    {
        return (m_slot ? m_slot->currentOwner() : nullptr) == other.owner();
    }

    // Identity of the slot. Not comparable with the owner() of the other pointer
    // types, that one names a block and the slot moves from block to block: use
    // == to ask whether the slot holds the object of a SharedPtr.
    constexpr const void *owner() const noexcept { return m_slot; }

    // Number of pointers sharing the slot
    constexpr std::size_t useCount() const noexcept
    {
        return m_slot ? m_slot->refs.load(std::memory_order_relaxed) : 0;
    }

    // Number of observers of the object the slot holds right now, plus one
    // collectively held by its owners. Observers watch an object, not the slot.
    constexpr std::size_t weakCount() const noexcept
    {
        return m_slot ? m_slot->currentWeakCount() : 0;
    }

    constexpr explicit operator bool() const noexcept { return m_slot != nullptr; }

    // Const: a write changes the object and, at most, the slot. The pointer stays
    // what it is, so this works on a temporary and on a const pointer alike.
    UniqueLockGuard<T, M> writeLock() const
    {
        SAFECPP_EXPECT(m_slot != nullptr, "SharedCopyOnWritePtr is null!");
        const auto pinned = m_slot->lockUnique();
        return UniqueLockGuard<T, M>(m_slot, pinned.cb, resolve(pinned.data));
    }

    SharedLockGuard<T, M> readLock() const
    {
        SAFECPP_EXPECT(m_slot != nullptr, "SharedCopyOnWritePtr is null!");
        const auto pinned = m_slot->lockShared();
        return SharedLockGuard<T, M>(m_slot, pinned.cb, resolve(pinned.data));
    }

    // An independent copy-on-write pointer to the current value. Nothing is cloned
    // here: the slot does that on its next write, the snapshot on its own.
    // Never waits. Taken while another thread holds a write lock, it shows the
    // value that write leaves behind.
    [[nodiscard]] constexpr CopyOnWritePtr<T, M> snapshot() const noexcept
    {
        if (!m_slot)
            return {};

        const auto shared = m_slot->share();
        CopyOnWritePtr<T, M> result(shared.cb, resolve(shared.data));
        if (shared.clone) {
            result.m_clone = shared.clone;
            result.m_cloneRoot = shared.cloneRoot;
            result.m_cloneOwner = shared.cb;
        }
        return result;
    }
};

template<typename T, SharedMutex M>
constexpr void swap(SharedCopyOnWritePtr<T, M> &first, SharedCopyOnWritePtr<T, M> &second) noexcept
{
    first.swap(second);
}

// The casts of a SharedCopyOnWritePtr stay on the slot: the result is another
// pointer of it, looking at the same object as a different type. A write through
// it clones like a write through any of the others, and all of them see it.
//
// An empty pointer comes back if the cast fails, and for one case that would
// work on the object as it is: an object adopted from a plain SharedPtr is
// cloned as the type it was adopted as, so a pointer to more than that type is
// refused. It would point past the clone.
template<typename T, typename U, SharedMutex M>
[[nodiscard]] constexpr SharedCopyOnWritePtr<T, M>
staticCast(const SharedCopyOnWritePtr<U, M> &other) noexcept
{
    return other.template castTo<T>([](U *pointer) { return static_cast<T *>(pointer); });
}

template<typename T, typename U, SharedMutex M>
[[nodiscard]] constexpr SharedCopyOnWritePtr<T, M>
dynamicCast(const SharedCopyOnWritePtr<U, M> &other) noexcept
{
    return other.template castTo<T>([](U *pointer) { return dynamic_cast<T *>(pointer); });
}

template<typename T, typename U, SharedMutex M>
[[nodiscard]] constexpr SharedCopyOnWritePtr<T, M>
constCast(const SharedCopyOnWritePtr<U, M> &other) noexcept
{
    return other.template castTo<T>([](U *pointer) { return const_cast<T *>(pointer); });
}

template<typename T, SharedMutex M, typename... Args>
    requires std::copy_constructible<T>
SharedCopyOnWritePtr<T, M> makeSharedCopyOnWrite(Args &&...args)
{
    return allocateSharedCopyOnWrite<T, M>(std::allocator<std::byte>{},
                                           std::forward<Args>(args)...);
}

// The object, its clones and the slot all come from allocator
template<typename T, SharedMutex M, typename Allocator, typename... Args>
    requires std::copy_constructible<T>
SharedCopyOnWritePtr<T, M> allocateSharedCopyOnWrite(const Allocator &allocator, Args &&...args)
{
    return SharedCopyOnWritePtr<T, M>(
        allocator, allocateCopyOnWrite<T, M>(allocator, std::forward<Args>(args)...));
}

// Watches a slot without keeping its object alive, the way a WeakPtr watches an
// object: lock() gives back a SharedCopyOnWritePtr for as long as a pointer of the
// slot exists, the CopyOnWritePtr it belongs to included.
//
//     safe::WeakCopyOnWritePtr<Item> observer = working[3];
//     ...
//     if (auto item = observer.lock()) // a SharedCopyOnWritePtr, or empty
//         item.writeLock()->name = "z"; // reaches working[3]
//
// It follows the slot to every clone. A WeakPtr made from the same pointer stays
// with the object the slot had, and its lock() gives a plain SharedPtr.
//
// Made from a CopyOnWritePtr it gives the pointer a slot if it has none, which
// is a read of the pointer, like making a SharedCopyOnWritePtr from it. Assigning
// to that pointer later makes it a different pointer: the observer stays with
// the slot it had, and expires with the last pointer of that slot.
template<typename T, SharedMutex M>
class WeakCopyOnWritePtr
{
    template<typename, SharedMutex>
    friend class WeakCopyOnWritePtr;

    using Slot = detail::CopyOnWriteSlot<M>;

    Slot *m_slot{nullptr};
    std::ptrdiff_t m_offset{0}; // see SharedCopyOnWritePtr

public:
    using ElementType = T;
    using MutexType = M;

    constexpr WeakCopyOnWritePtr() = default;

    template<typename U>
        requires std::is_convertible_v<U *, T *>
    constexpr WeakCopyOnWritePtr(const SharedCopyOnWritePtr<U, M> &shared) noexcept
    {
        if constexpr (std::is_same_v<U, T>) {
            m_slot = shared.m_slot;
            m_offset = shared.m_offset;
        } else {
            // The pointer that knows where the base class is, asked of the object
            const SharedCopyOnWritePtr<T, M> converted(shared);
            m_slot = converted.m_slot;
            m_offset = converted.m_offset;
        }
        if (m_slot)
            m_slot->addWeak();
    }

    template<typename U>
        requires std::is_convertible_v<U *, T *>
    WeakCopyOnWritePtr(const CopyOnWritePtr<U, M> &pointer)
        : WeakCopyOnWritePtr(SharedCopyOnWritePtr<T, M>(pointer))
    {}

    constexpr WeakCopyOnWritePtr(const WeakCopyOnWritePtr &other) noexcept
        : m_slot(other.m_slot)
        , m_offset(other.m_offset)
    {
        if (m_slot)
            m_slot->addWeak();
    }

    constexpr WeakCopyOnWritePtr(WeakCopyOnWritePtr &&other) noexcept
        : m_slot(std::exchange(other.m_slot, nullptr))
        , m_offset(std::exchange(other.m_offset, 0))
    {}

    // Implicit conversion to a compatible pointer type (e.g. Derived -> Base).
    // Where a base class is can only be asked of a live object, so an observer
    // whose slot has lost its last pointer converts to an empty one.
    template<typename U>
        requires(!std::is_same_v<U, T> && std::is_convertible_v<U *, T *>)
    constexpr WeakCopyOnWritePtr(const WeakCopyOnWritePtr<U, M> &other) noexcept
        : WeakCopyOnWritePtr(SharedCopyOnWritePtr<T, M>(other.lock()))
    {}

    constexpr ~WeakCopyOnWritePtr()
    {
        if (m_slot)
            m_slot->releaseWeak();
    }

    constexpr WeakCopyOnWritePtr &operator=(const WeakCopyOnWritePtr &other) noexcept
    {
        if (this != &other)
            WeakCopyOnWritePtr(other).swap(*this);
        return *this;
    }

    constexpr WeakCopyOnWritePtr &operator=(WeakCopyOnWritePtr &&other) noexcept
    {
        if (this != &other)
            WeakCopyOnWritePtr(std::move(other)).swap(*this);
        return *this;
    }

    // Everything a WeakCopyOnWritePtr can be made from can be assigned too
    template<typename Other>
        requires(!std::is_same_v<std::remove_cvref_t<Other>, WeakCopyOnWritePtr>
                 && std::is_convertible_v<Other, WeakCopyOnWritePtr>)
    constexpr WeakCopyOnWritePtr &operator=(Other &&other) noexcept(
        std::is_nothrow_constructible_v<WeakCopyOnWritePtr, Other>)
    {
        WeakCopyOnWritePtr(std::forward<Other>(other)).swap(*this);
        return *this;
    }

    constexpr void reset() noexcept { WeakCopyOnWritePtr().swap(*this); }

    constexpr void swap(WeakCopyOnWritePtr &other) noexcept
    {
        std::swap(m_slot, other.m_slot);
        std::swap(m_offset, other.m_offset);
    }

    // Ownership comparison: equal if they watch one slot, whichever base class
    // they look at it through. An expired observer keeps comparing equal to its
    // peers.
    template<typename U>
    constexpr bool operator==(const WeakCopyOnWritePtr<U, M> &other) const noexcept
    {
        return m_slot == other.m_slot;
    }

    template<typename U>
    constexpr std::strong_ordering operator<=>(const WeakCopyOnWritePtr<U, M> &other) const noexcept
    {
        return std::compare_three_way{}(m_slot, other.m_slot);
    }

    // Identity of the slot, comparable with SharedCopyOnWritePtr::owner()
    constexpr const void *owner() const noexcept { return m_slot; }

    // Number of pointers sharing the slot
    constexpr std::size_t useCount() const noexcept
    {
        return m_slot ? m_slot->refs.load(std::memory_order_relaxed) : 0;
    }

    constexpr bool expired() const noexcept { return useCount() == 0; }

    // Another pointer of the slot, or an empty one once the slot has lost its
    // last pointer
    [[nodiscard]] constexpr SharedCopyOnWritePtr<T, M> lock() const noexcept
    {
        if (m_slot && m_slot->tryAddRef())
            return SharedCopyOnWritePtr<T, M>(m_slot, m_offset);
        return {};
    }
};

template<typename T, SharedMutex M>
constexpr void swap(WeakCopyOnWritePtr<T, M> &first, WeakCopyOnWritePtr<T, M> &second) noexcept
{
    first.swap(second);
}

#if SAFECPP_THROW_ON_NULL
} // namespace throwOnNull
#endif
} // namespace safe

#undef SAFECPP_EXPECT

// Hashing is by owner, exactly like ==, so the pointers work as keys in the
// unordered containers. A SharedPtr and a WeakPtr to the same object hash the
// same, and an expired WeakPtr keeps the hash it had.
//
// CopyOnWritePtr is deliberately left unhashable: detaching moves it to a fresh
// control block, so a key already sitting in a container would silently stop
// matching the pointer it was made from. Convert it to a SharedPtr, and mean it,
// if you want to key on the object it owns right now.
//
// A SharedCopyOnWritePtr hashes its slot, which stays the same when the object is
// cloned, so it has no such problem. A WeakCopyOnWritePtr hashes the slot it
// watches, like the pointers of that slot.
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

template<typename T, safe::SharedMutex M>
struct hash<safe::SharedCopyOnWritePtr<T, M>>
{
    std::size_t operator()(const safe::SharedCopyOnWritePtr<T, M> &pointer) const noexcept
    {
        return std::hash<const void *>{}(pointer.owner());
    }
};

template<typename T, safe::SharedMutex M>
struct hash<safe::WeakCopyOnWritePtr<T, M>>
{
    std::size_t operator()(const safe::WeakCopyOnWritePtr<T, M> &pointer) const noexcept
    {
        return std::hash<const void *>{}(pointer.owner());
    }
};

} // namespace std
