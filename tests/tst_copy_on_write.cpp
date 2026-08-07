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

using namespace tst;

TEST(CopyOnWrite, staysOnTheSameBlockWhileItIsTheOnlyOwner)
{
    Tracked::resetCounters();
    auto pointer = safe::makeCopyOnWrite<Tracked>(1);
    const void *controlBlock = pointer.owner();

    pointer.writeLock()->value = 2;
    CHECK(pointer.owner() == controlBlock);
    CHECK_EQ(Tracked::copied.load(), 0);
    CHECK_EQ(pointer.readLock()->value, 2);
}

TEST(CopyOnWrite, detachesOnTheFirstWriteWhileShared)
{
    Tracked::resetCounters();
    auto original = safe::makeCopyOnWrite<Tracked>(1);
    auto copy = original;
    const void *sharedBlock = original.owner();
    CHECK_EQ(sharedCount(original), size_t(2));

    copy.writeLock()->value = 2;

    CHECK(copy.owner() != sharedBlock);
    CHECK(original.owner() == sharedBlock);
    CHECK_EQ(Tracked::copied.load(), 1);
    CHECK_EQ(sharedCount(original), size_t(1));
    CHECK_EQ(sharedCount(copy), size_t(1));
    CHECK_EQ(original.readLock()->value, 1);
    CHECK_EQ(copy.readLock()->value, 2);
}

TEST(CopyOnWrite, detachesOnlyOnce)
{
    Tracked::resetCounters();
    auto original = safe::makeCopyOnWrite<Tracked>(1);
    auto copy = original;

    copy.writeLock()->value = 2;
    const void *detachedBlock = copy.owner();
    copy.writeLock()->value = 3;

    CHECK(copy.owner() == detachedBlock);
    CHECK_EQ(Tracked::copied.load(), 1);
    CHECK_EQ(copy.readLock()->value, 3);
}

TEST(CopyOnWrite, sharedLockDoesNotDetach)
{
    Tracked::resetCounters();
    auto original = safe::makeCopyOnWrite<Tracked>(1);
    auto copy = original;

    CHECK_EQ(copy.readLock()->value, 1);
    CHECK(copy.owner() == original.owner());
    CHECK_EQ(Tracked::copied.load(), 0);
}

TEST(CopyOnWrite, destroysTheDetachedCopy)
{
    Tracked::resetCounters();
    {
        auto original = safe::makeCopyOnWrite<Tracked>(1);
        {
            auto copy = original;
            copy.writeLock()->value = 2;
            CHECK_EQ(Tracked::alive.load(), 2);
        }
        CHECK_EQ(Tracked::alive.load(), 1);
    }
    CHECK_EQ(Tracked::alive.load(), 0);
}

TEST(CopyOnWrite, weakObserversFollowTheOriginalBlock)
{
    auto original = safe::makeCopyOnWrite<Tracked>(1);
    safe::WeakPtr<Tracked> weak = original;
    auto copy = original;

    copy.writeLock()->value = 2;

    auto locked = weak.lock();
    REQUIRE(bool(locked));
    CHECK_EQ(locked.readLock()->value, 1); // the weak reference kept watching the original
    CHECK(locked.owner() == original.owner());
}

TEST(CopyOnWrite, adoptsAnExistingSharedPointer)
{
    Tracked::resetCounters();
    auto shared = safe::makeShared<Tracked>(1);
    safe::CopyOnWritePtr<Tracked> cow = shared;
    CHECK_EQ(sharedCount(shared), size_t(2));

    cow.writeLock()->value = 2;
    CHECK_EQ(shared.readLock()->value, 1);
    CHECK_EQ(cow.readLock()->value, 2);
    CHECK_EQ(Tracked::copied.load(), 1);
}

TEST(CopyOnWrite, worksWithADerivedType)
{
    auto original = safe::makeCopyOnWrite<Derived>();
    auto copy = original;

    copy.writeLock()->derived = 22;
    CHECK_EQ(original.readLock()->derived, 2);
    CHECK_EQ(copy.readLock()->derived, 22);
    CHECK_EQ(copy.readLock()->base, 1);
}

TEST(CopyOnWrite, castingBackFromABaseViewKeepsTheSameObject)
{
    auto original = safe::makeCopyOnWrite<Derived>();
    safe::SharedPtr<Base> base = original;

    auto back = safe::dynamicCast<Derived>(base);
    REQUIRE(bool(back));
    CHECK(back.owner() == original.owner());
    CHECK_EQ(back.readLock()->derived, 2);
}

TEST(CopyOnWrite, lockThroughASharedPointerReferenceStillDetaches)
{
    // lock() is virtual, so copy-on-write semantics survive being reached through
    // a base reference that has no idea it is looking at a CopyOnWritePtr
    Tracked::resetCounters();
    auto original = safe::makeCopyOnWrite<Tracked>(1);
    auto copy = original;

    safe::SharedPtr<Tracked> &asBase = copy;
    asBase.writeLock()->value = 2;

    CHECK_EQ(Tracked::copied.load(), 1);
    CHECK(copy.owner() != original.owner());
    CHECK_EQ(original.readLock()->value, 1);
    CHECK_EQ(copy.readLock()->value, 2);
}

TEST(CopyOnWrite, aPlainSharedCopyKeepsSharedSemantics)
{
    // Converting to a SharedPtr is an explicit request for a shared view, so
    // writing through that separate object must not detach
    Tracked::resetCounters();
    auto original = safe::makeCopyOnWrite<Tracked>(1);
    safe::SharedPtr<Tracked> shared = original;

    shared.writeLock()->value = 2;

    CHECK_EQ(Tracked::copied.load(), 0);
    CHECK(shared.owner() == original.owner());
    CHECK_EQ(original.readLock()->value, 2);
}

TEST(CopyOnWrite, guardOutlivingItsPointerKeepsTheDetachedCopyAlive)
{
    Tracked::resetCounters();
    {
        auto original = safe::makeCopyOnWrite<Tracked>(1);
        auto copy = original;

        auto guard = copy.writeLock(); // detaches, then locks the fresh block
        copy = safe::CopyOnWritePtr<Tracked>{};
        CHECK_EQ(Tracked::alive.load(), 2); // the original plus the detached copy

        guard->value = 2;
        CHECK_EQ(guard->value, 2);
        CHECK_EQ(original.readLock()->value, 1);
    }
    CHECK_EQ(Tracked::alive.load(), 0);
}
