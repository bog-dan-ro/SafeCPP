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

#include <memory>
#include <utility>
#include <vector>

using namespace tst;

TEST(SharedPtr, defaultConstructedIsEmpty)
{
    safe::SharedPtr<int> pointer;
    CHECK(!pointer);
    CHECK(pointer.owner() == nullptr);
}

TEST(SharedPtr, makeSharedForwardsArguments)
{
    auto pointer = safe::makeShared<Tracked>(42, "answer");
    REQUIRE(bool(pointer));
    CHECK_EQ(pointer.readLock()->value, 42);
    CHECK_EQ(pointer.readLock()->tag, std::string("answer"));
    CHECK_EQ(sharedCount(pointer), size_t(1));
    CHECK_EQ(weakCount(pointer), size_t(1));
}

TEST(SharedPtr, makeSharedAcceptsMoveOnlyArguments)
{
    auto pointer = safe::makeShared<MoveOnly>(7);
    CHECK_EQ(pointer.readLock()->value, 7);
}

TEST(SharedPtr, copyingSharesOwnership)
{
    auto first = safe::makeShared<Tracked>(1);
    {
        auto second = first;
        CHECK_EQ(sharedCount(first), size_t(2));
        CHECK(first.owner() == second.owner());
        *second.writeLock() = Tracked{5};
        CHECK_EQ(first.readLock()->value, 5);
    }
    CHECK_EQ(sharedCount(first), size_t(1));
}

TEST(SharedPtr, movingLeavesTheSourceEmpty)
{
    auto first = safe::makeShared<Tracked>(1);
    const void *controlBlock = first.owner();

    auto second = std::move(first);
    CHECK(!first);
    REQUIRE(bool(second));
    CHECK(second.owner() == controlBlock);
    CHECK_EQ(sharedCount(second), size_t(1));
}

TEST(SharedPtr, copyAssignmentReleasesThePreviousObject)
{
    Tracked::resetCounters();
    {
        auto first = safe::makeShared<Tracked>(1);
        auto second = safe::makeShared<Tracked>(2);
        CHECK_EQ(Tracked::alive.load(), 2);

        second = first;
        CHECK_EQ(Tracked::alive.load(), 1);
        CHECK_EQ(sharedCount(first), size_t(2));
        CHECK_EQ(second.readLock()->value, 1);
    }
    CHECK_EQ(Tracked::alive.load(), 0);
}

TEST(SharedPtr, moveAssignmentReleasesThePreviousObject)
{
    Tracked::resetCounters();
    {
        auto first = safe::makeShared<Tracked>(1);
        auto second = safe::makeShared<Tracked>(2);

        second = std::move(first);
        CHECK(!first);
        CHECK_EQ(Tracked::alive.load(), 1);
        CHECK_EQ(sharedCount(second), size_t(1));
        CHECK_EQ(second.readLock()->value, 1);
    }
    CHECK_EQ(Tracked::alive.load(), 0);
}

TEST(SharedPtr, selfAssignmentKeepsTheObjectAlive)
{
    Tracked::resetCounters();
    auto pointer = safe::makeShared<Tracked>(3);
    auto &alias = pointer; // through an alias, so the compiler cannot fold it away

    pointer = alias;
    CHECK_EQ(Tracked::alive.load(), 1);
    CHECK_EQ(sharedCount(pointer), size_t(1));

    pointer = std::move(alias);
    CHECK_EQ(Tracked::alive.load(), 1);
    CHECK_EQ(pointer.readLock()->value, 3);
}

TEST(SharedPtr, indirectSelfAssignmentDoesNotDestroyTheSourceBeforeCopying)
{
    struct Chain
    {
        Tracked tracked;
        safe::SharedPtr<Chain> next;

        explicit Chain(int value)
            : tracked(value)
        {}
    };

    Tracked::resetCounters();
    {
        auto head = safe::makeShared<Chain>(1);
        const safe::SharedPtr<Chain> *nextAddress = nullptr;
        {
            auto guard = head.writeLock();
            guard->next = safe::makeShared<Chain>(2);
            nextAddress = &guard->next;
        }
        CHECK_EQ(Tracked::alive.load(), 2);

        // head is the sole owner of *nextAddress; releasing head before copying
        // *nextAddress would destroy *nextAddress mid-assignment.
        head = *nextAddress;
        CHECK_EQ(Tracked::alive.load(), 1);
        CHECK_EQ(head.readLock()->tracked.value, 2);
    }
    CHECK_EQ(Tracked::alive.load(), 0);
}

TEST(SharedPtr, destroysTheObjectWithTheLastOwner)
{
    Tracked::resetCounters();
    {
        auto first = safe::makeShared<Tracked>(1);
        auto second = first;
        auto third = second;
        CHECK_EQ(sharedCount(first), size_t(3));
        CHECK_EQ(Tracked::alive.load(), 1);
    }
    CHECK_EQ(Tracked::alive.load(), 0);
    CHECK_EQ(Tracked::constructed.load(), 1);
    CHECK_EQ(Tracked::copied.load(), 0);
}

TEST(SharedPtr, uniqueLockGrantsMutableAccess)
{
    auto pointer = safe::makeShared<Tracked>(1);
    pointer.writeLock()->value = 11;
    CHECK_EQ(pointer.readLock()->value, 11);

    {
        auto guard = pointer.writeLock();
        guard->value = 12;
        (*guard).value += 1;
    }
    CHECK_EQ(pointer.readLock()->value, 13);
}

TEST(SharedPtr, guardsUseTheExpectedMutexOperations)
{
    InstrumentedMutex::resetCounters();
    auto pointer = safe::makeShared<Tracked, InstrumentedMutex>(1);

    {
        auto guard = pointer.readLock();
        CHECK_EQ(InstrumentedMutex::sharedLocks.load(), 1);
        CHECK_EQ(InstrumentedMutex::sharedUnlocks.load(), 0);
        CHECK_EQ(guard->value, 1);
    }
    CHECK_EQ(InstrumentedMutex::sharedUnlocks.load(), 1);
    CHECK_EQ(InstrumentedMutex::locks.load(), 0);

    {
        auto guard = pointer.writeLock();
        CHECK_EQ(InstrumentedMutex::locks.load(), 1);
        CHECK_EQ(InstrumentedMutex::unlocks.load(), 0);
    }
    CHECK_EQ(InstrumentedMutex::unlocks.load(), 1);
}

TEST(SharedPtr, movedGuardUnlocksExactlyOnce)
{
    InstrumentedMutex::resetCounters();
    auto pointer = safe::makeShared<Tracked, InstrumentedMutex>(1);

    {
        auto guard = pointer.writeLock();
        auto moved = std::move(guard);
        moved->value = 2;
    }
    CHECK_EQ(InstrumentedMutex::locks.load(), 1);
    CHECK_EQ(InstrumentedMutex::unlocks.load(), 1);

    {
        auto guard = pointer.readLock();
        auto moved = std::move(guard);
        CHECK_EQ(moved->value, 2);
    }
    CHECK_EQ(InstrumentedMutex::sharedLocks.load(), 1);
    CHECK_EQ(InstrumentedMutex::sharedUnlocks.load(), 1);
}

TEST(SharedPtr, eachLockCallIsItsOwnCriticalSection)
{
    InstrumentedMutex::resetCounters();
    auto pointer = safe::makeShared<Tracked, InstrumentedMutex>(1);

    // Two temporaries, so two separate critical sections
    pointer.writeLock()->value = 2;
    pointer.writeLock()->value = 3;
    CHECK_EQ(InstrumentedMutex::locks.load(), 2);
    CHECK_EQ(InstrumentedMutex::unlocks.load(), 2);
    CHECK_EQ(pointer.readLock()->value, 3);
}

TEST(SharedPtr, guardHoldsAReferenceWhileItLives)
{
    auto pointer = safe::makeShared<Tracked>(1);
    CHECK_EQ(sharedCount(pointer), size_t(1));

    {
        auto guard = pointer.writeLock();
        CHECK_EQ(sharedCount(pointer), size_t(2));
    }
    CHECK_EQ(sharedCount(pointer), size_t(1));

    {
        auto guard = pointer.readLock();
        CHECK_EQ(sharedCount(pointer), size_t(2));
    }
    CHECK_EQ(sharedCount(pointer), size_t(1));
}

TEST(SharedPtr, guardOutlivingItsPointerKeepsTheObjectAlive)
{
    Tracked::resetCounters();
    {
        auto pointer = safe::makeShared<Tracked>(1);
        auto guard = pointer.writeLock();

        pointer = safe::SharedPtr<Tracked>{}; // the guard is now the only owner
        CHECK(!pointer);
        CHECK_EQ(Tracked::alive.load(), 1);

        guard->value = 5;
        CHECK_EQ(guard->value, 5);
    }
    CHECK_EQ(Tracked::alive.load(), 0);
}

TEST(SharedPtr, sharedGuardOutlivingItsPointerKeepsTheObjectAlive)
{
    Tracked::resetCounters();
    {
        auto guard = safe::makeShared<Tracked>(7).readLock(); // temporary pointer
        CHECK_EQ(Tracked::alive.load(), 1);
        CHECK_EQ(guard->value, 7);
    }
    CHECK_EQ(Tracked::alive.load(), 0);
}

TEST(SharedPtr, movedGuardReleasesItsReferenceExactlyOnce)
{
    auto pointer = safe::makeShared<Tracked>(1);
    {
        auto guard = pointer.writeLock();
        auto moved = std::move(guard);
        CHECK_EQ(sharedCount(pointer), size_t(2));
    }
    CHECK_EQ(sharedCount(pointer), size_t(1));
}

namespace {

// std::is_copy_constructible is true for this type, yet copying it does not
// compile: the trait only sees the declaration of the vector's copy constructor
struct DeepOwner
{
    std::vector<std::unique_ptr<int>> values;
};
static_assert(std::is_copy_constructible_v<DeepOwner>);

} // namespace

TEST(SharedPtr, doesNotNeedACopyConstructor)
{
    // Copying is a CopyOnWritePtr concern. None of these may instantiate a copy
    // constructor, which for DeepOwner is a hard compile error.
    auto made = safe::makeShared<DeepOwner>();
    made.writeLock()->values.push_back(std::make_unique<int>(7));
    CHECK_EQ(*made.readLock()->values.front(), 7);

    safe::SharedPtr<DeepOwner> adopted(new DeepOwner);
    CHECK(adopted.readLock()->values.empty());

    auto allocated = safe::allocateShared<DeepOwner>(std::allocator<std::byte>{});
    CHECK(allocated.readLock()->values.empty());

    auto moveOnly = safe::makeShared<MoveOnly>(3);
    CHECK_EQ(moveOnly.readLock()->value, 3);
}

TEST(SharedPtr, ownsAnIncompleteTypeThroughACustomDeleter)
{
    {
        safe::SharedPtr<Opaque> handle(createOpaque(11),
                                       [](Opaque *opaque) noexcept { destroyOpaque(opaque); });
        auto copy = handle;
        CHECK_EQ(opaqueAlive(), 1);
        CHECK_EQ(opaqueValue(&*copy.readLock()), 11);
    }
    CHECK_EQ(opaqueAlive(), 0);
}
