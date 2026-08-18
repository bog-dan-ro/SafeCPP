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

#include "TestHarness.h"
#include "TestTypes.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <utility>

using namespace tst;

namespace {

struct AllocationStats
{
    int allocations = 0;
    int deallocations = 0;
    int trackedConstructions = 0;
    int trackedDestructions = 0;
    bool failAllocation = false;
};

template<typename T>
class CountingAllocator
{
    template<typename>
    friend class CountingAllocator;

public:
    using value_type = T;

    AllocationStats *stats = nullptr;

    explicit CountingAllocator(AllocationStats &stats) noexcept
        : stats(&stats)
    {}

    template<typename U>
    CountingAllocator(const CountingAllocator<U> &other) noexcept
        : stats(other.stats)
    {}

    T *allocate(std::size_t count)
    {
        if (stats->failAllocation)
            throw std::bad_alloc{};
        ++stats->allocations;
        return std::allocator<T>{}.allocate(count);
    }

    void deallocate(T *pointer, std::size_t count) noexcept
    {
        ++stats->deallocations;
        std::allocator<T>{}.deallocate(pointer, count);
    }

    template<typename U, typename... Args>
    void construct(U *pointer, Args &&...args)
    {
        std::construct_at(pointer, std::forward<Args>(args)...);
        if constexpr (std::is_same_v<std::remove_cv_t<U>, Tracked>)
            ++stats->trackedConstructions;
    }

    template<typename U>
    void destroy(U *pointer) noexcept
    {
        if constexpr (std::is_same_v<std::remove_cv_t<U>, Tracked>)
            ++stats->trackedDestructions;
        std::destroy_at(pointer);
    }

    template<typename U>
    bool operator==(const CountingAllocator<U> &other) const noexcept
    {
        return stats == other.stats;
    }
};

struct CountingDeleter
{
    int *calls = nullptr;

    void operator()(Tracked *pointer) const noexcept
    {
        ++*calls;
        delete pointer;
    }
};

struct ThrowingCopyDeleter
{
    int *calls = nullptr;

    explicit ThrowingCopyDeleter(int &calls) noexcept
        : calls(&calls)
    {}

    ThrowingCopyDeleter(const ThrowingCopyDeleter &other)
        : calls(other.calls)
    {
        throw 73;
    }

    ThrowingCopyDeleter(ThrowingCopyDeleter &&) noexcept = default;
    ThrowingCopyDeleter &operator=(const ThrowingCopyDeleter &) = default;

    void operator()(Tracked *pointer) const noexcept
    {
        ++*calls;
        delete pointer;
    }
};

struct PotentiallyThrowingDeleter
{
    void operator()(Tracked *pointer) const { delete pointer; }
};

struct ThrowingConstructor
{
    ThrowingConstructor() { throw 42; }
};

struct alignas(128) OverAligned
{
    int value = 9;
};

struct NonVirtualBase
{
    static inline int destroyed = 0;
    ~NonVirtualBase() { ++destroyed; }
};

struct NonVirtualDerived : NonVirtualBase
{
    static inline int destroyed = 0;
    ~NonVirtualDerived() { ++destroyed; }
};

struct RawNode : safe::EnableSharedFromThis<RawNode>
{
    int value = 3;
};

struct RawDerivedNode : RawNode
{
    int extra = 4;
};

struct RawOwnerBase
{
    virtual ~RawOwnerBase() = default;
};

struct RawOwnedNode : RawOwnerBase, safe::EnableSharedFromThis<RawOwnedNode>
{
    int value = 8;
};

} // namespace

static_assert(
    !std::is_constructible_v<safe::SharedPtr<Tracked>, Tracked *, PotentiallyThrowingDeleter>);
static_assert(
    !std::is_constructible_v<safe::SharedPtr<Tracked>, Tracked *, std::default_delete<Tracked>>);

TEST(Allocation, allocateSharedUsesOneAllocatorBackedBlock)
{
    Tracked::resetCounters();
    AllocationStats stats;
    safe::WeakPtr<Tracked> observer;

    {
        auto pointer =
            safe::allocateShared<Tracked>(CountingAllocator<std::byte>(stats), 7, "allocated");
        observer = pointer;

        CHECK_EQ(stats.allocations, 1);
        CHECK_EQ(stats.deallocations, 0);
        CHECK_EQ(stats.trackedConstructions, 1);
        CHECK_EQ(pointer.readLock()->value, 7);
        CHECK_EQ(pointer.readLock()->tag, std::string("allocated"));
    }

    CHECK_EQ(Tracked::alive.load(), 0);
    CHECK_EQ(stats.trackedDestructions, 1);
    CHECK_EQ(stats.deallocations, 0); // the weak observer still owns the block

    observer.reset();
    CHECK_EQ(stats.deallocations, 1);
}

TEST(Allocation, allocateSharedRollsBackAThrowingConstruction)
{
    AllocationStats stats;

    try {
        auto pointer =
            safe::allocateShared<ThrowingConstructor>(CountingAllocator<std::byte>(stats));
        CHECK(bool(pointer));
    } catch (int value) {
        CHECK_EQ(value, 42);
    }

    CHECK_EQ(stats.allocations, 1);
    CHECK_EQ(stats.deallocations, 1);
}

TEST(Allocation, allocateSharedPreservesOverAlignment)
{
    auto pointer = safe::allocateShared<OverAligned>(std::allocator<std::byte>{});
    auto guard = pointer.readLock();

    CHECK_EQ(reinterpret_cast<std::uintptr_t>(&*guard) % alignof(OverAligned),
             std::uintptr_t(0));
    CHECK_EQ(guard->value, 9);
}

TEST(Allocation, rawPointerKeepsTheDynamicDeletionType)
{
    NonVirtualBase::destroyed = 0;
    NonVirtualDerived::destroyed = 0;

    {
        safe::SharedPtr<NonVirtualBase> pointer(new NonVirtualDerived);
        CHECK(bool(pointer));
    }

    CHECK_EQ(NonVirtualDerived::destroyed, 1);
    CHECK_EQ(NonVirtualBase::destroyed, 1);
}

TEST(Allocation, customDeleterIsStoredAndQueryableThroughAliases)
{
    Tracked::resetCounters();
    int calls = 0;

    {
        safe::SharedPtr<Tracked> pointer(new Tracked(4), CountingDeleter{&calls});
        int *member = nullptr;
        {
            auto guard = pointer.writeLock();
            member = &guard->value;
        }
        safe::SharedPtr<int> alias(pointer, member);

        auto *deleter = safe::getDeleter<CountingDeleter>(alias);
        REQUIRE(deleter != nullptr);
        CHECK(deleter->calls == &calls);
        CHECK(safe::getDeleter<std::default_delete<Tracked>>(pointer) == nullptr);
    }

    CHECK_EQ(calls, 1);
    CHECK_EQ(Tracked::alive.load(), 0);
}

TEST(Allocation, externalControlBlockUsesTheSuppliedAllocator)
{
    Tracked::resetCounters();
    AllocationStats stats;
    int calls = 0;
    safe::WeakPtr<Tracked> observer;

    {
        safe::SharedPtr<Tracked> pointer(new Tracked(5),
                                        CountingDeleter{&calls},
                                        CountingAllocator<std::byte>(stats));
        observer = pointer;
        CHECK_EQ(stats.allocations, 1);
    }

    CHECK_EQ(calls, 1);
    CHECK_EQ(Tracked::alive.load(), 0);
    CHECK_EQ(stats.deallocations, 0);

    observer.reset();
    CHECK_EQ(stats.deallocations, 1);
}

TEST(Allocation, allocationFailureInvokesTheCustomDeleter)
{
    Tracked::resetCounters();
    AllocationStats stats;
    stats.failAllocation = true;
    int calls = 0;

    try {
        safe::SharedPtr<Tracked> pointer(new Tracked(6),
                                        CountingDeleter{&calls},
                                        CountingAllocator<std::byte>(stats));
        CHECK(bool(pointer));
    } catch (const std::bad_alloc &) {
        CHECK(true);
    }

    CHECK_EQ(calls, 1);
    CHECK_EQ(Tracked::alive.load(), 0);
    CHECK_EQ(stats.deallocations, 0);
}

TEST(Allocation, deleterCopyFailureCleansUpThePointerAndBlock)
{
    Tracked::resetCounters();
    AllocationStats stats;
    int calls = 0;

    try {
        safe::SharedPtr<Tracked> pointer(new Tracked(7),
                                        ThrowingCopyDeleter(calls),
                                        CountingAllocator<std::byte>(stats));
        CHECK(bool(pointer));
    } catch (int value) {
        CHECK_EQ(value, 73);
    }

    CHECK_EQ(calls, 1);
    CHECK_EQ(Tracked::alive.load(), 0);
    CHECK_EQ(stats.allocations, 1);
    CHECK_EQ(stats.deallocations, 1);
}

TEST(Allocation, resetCanAdoptANewObjectAndDeleter)
{
    Tracked::resetCounters();
    int calls = 0;

    {
        safe::SharedPtr<Tracked> pointer(new Tracked(1));
        pointer.reset(new Tracked(2), CountingDeleter{&calls});

        CHECK_EQ(Tracked::alive.load(), 1);
        CHECK_EQ(pointer.readLock()->value, 2);
    }

    CHECK_EQ(calls, 1);
    CHECK_EQ(Tracked::alive.load(), 0);
}

TEST(Allocation, rawOwnershipArmsSharedFromThis)
{
    safe::SharedPtr<RawNode> pointer(new RawNode);

    auto self = pointer.readLock()->sharedFromThis();
    REQUIRE(bool(self));
    CHECK(self == pointer);
    CHECK_EQ(self.readLock()->value, 3);
}

TEST(Allocation, rawOwnershipArmsSharedFromThisThroughADerivedObject)
{
    safe::SharedPtr<RawNode> pointer(new RawDerivedNode);

    auto self = pointer.readLock()->sharedFromThis();
    REQUIRE(bool(self));
    CHECK(self == pointer);
    CHECK_EQ(self.readLock()->value, 3);
}

TEST(Allocation, rawOwnershipArmsAnUnrelatedBaseView)
{
    safe::SharedPtr<RawOwnerBase> pointer(new RawOwnedNode);
    auto derived = safe::dynamicCast<RawOwnedNode>(pointer);
    REQUIRE(bool(derived));

    auto self = derived.readLock()->sharedFromThis();
    REQUIRE(bool(self));
    CHECK(self == pointer);
    CHECK_EQ(self.readLock()->value, 8);
}

TEST(Allocation, copyOnWriteDetachesFromAnExternalObject)
{
    Tracked::resetCounters();
    int calls = 0;

    {
        safe::SharedPtr<Tracked> shared(new Tracked(1), CountingDeleter{&calls});
        safe::CopyOnWritePtr<Tracked> copy = shared;

        copy.writeLock()->value = 2;
        CHECK(copy.owner() != shared.owner());
        CHECK_EQ(shared.readLock()->value, 1);
        CHECK_EQ(copy.readLock()->value, 2);
    }

    CHECK_EQ(calls, 1);
    CHECK_EQ(Tracked::alive.load(), 0);
}
