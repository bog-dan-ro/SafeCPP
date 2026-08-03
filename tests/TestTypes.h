#pragma once

#include "SafeCpp.h"

#include <atomic>
#include <shared_mutex>
#include <string>
#include <utility>

namespace tst {

// Counts live instances so tests can detect leaked or double destroyed objects
struct Tracked
{
    static inline std::atomic<int> alive{0};
    static inline std::atomic<int> constructed{0};
    static inline std::atomic<int> copied{0};

    int value = 0;
    std::string tag;

    explicit Tracked(int value = 0, std::string tag = {})
        : value(value)
        , tag(std::move(tag))
    {
        ++alive;
        ++constructed;
    }

    Tracked(const Tracked &other)
        : value(other.value)
        , tag(other.tag)
    {
        ++alive;
        ++constructed;
        ++copied;
    }

    Tracked &operator=(const Tracked &) = default;

    ~Tracked() { --alive; }

    static void resetCounters()
    {
        alive = 0;
        constructed = 0;
        copied = 0;
    }
};

struct MoveOnly
{
    int value = 0;

    explicit MoveOnly(int value)
        : value(value)
    {}

    MoveOnly(MoveOnly &&) = default;
    MoveOnly(const MoveOnly &) = delete;
};

// Single inheritance hierarchy
struct Base
{
    virtual ~Base() = default;
    int base = 1;
};

struct Derived : Base
{
    int derived = 2;
};

struct Unrelated : Base
{
    int unrelated = 3;
};

// Multiple inheritance: casting between Left/Right/Both shifts the address, which
// only works if the pointer stores the adjusted object address next to the block
struct Left
{
    virtual ~Left() = default;
    int left = 10;
};

struct Right
{
    virtual ~Right() = default;
    int right = 20;
};

struct Both : Left, Right
{
    int both = 30;
};

// Satisfies the mutex requirements of safe::SharedPtr and records how it is used
class InstrumentedMutex
{
public:
    static inline std::atomic<int> locks{0};
    static inline std::atomic<int> unlocks{0};
    static inline std::atomic<int> sharedLocks{0};
    static inline std::atomic<int> sharedUnlocks{0};

    void lock()
    {
        m_mutex.lock();
        ++locks;
    }

    void unlock()
    {
        ++unlocks;
        m_mutex.unlock();
    }

    void lock_shared()
    {
        m_mutex.lock_shared();
        ++sharedLocks;
    }

    void unlock_shared()
    {
        ++sharedUnlocks;
        m_mutex.unlock_shared();
    }

    static void resetCounters()
    {
        locks = 0;
        unlocks = 0;
        sharedLocks = 0;
        sharedUnlocks = 0;
    }

private:
    std::shared_mutex m_mutex;
};

template<typename P>
size_t sharedCount(const P &pointer)
{
    return pointer.useCount();
}

template<typename P>
size_t weakCount(const P &pointer)
{
    return pointer.weakCount();
}

} // namespace tst
