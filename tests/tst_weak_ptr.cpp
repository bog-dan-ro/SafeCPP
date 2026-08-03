#include "TestHarness.h"
#include "TestTypes.h"

#include <utility>

using namespace tst;

TEST(WeakPtr, defaultConstructedIsExpired)
{
    safe::WeakPtr<int> weak;
    CHECK(weak.expired());
    CHECK(!weak.lock());
}

TEST(WeakPtr, locksWhileAnOwnerExists)
{
    auto owner = safe::makeShared<Tracked>(4);
    safe::WeakPtr<Tracked> weak = owner;

    CHECK(!weak.expired());
    CHECK_EQ(weakCount(owner), size_t(2)); // the implicit one plus ours
    CHECK_EQ(sharedCount(owner), size_t(1));

    auto locked = weak.lock();
    REQUIRE(bool(locked));
    CHECK_EQ(sharedCount(owner), size_t(2));
    CHECK_EQ(locked.readLock()->value, 4);
    CHECK(locked.owner() == owner.owner());
}

TEST(WeakPtr, expiresWhenTheLastOwnerGoes)
{
    Tracked::resetCounters();
    safe::WeakPtr<Tracked> weak;
    {
        auto owner = safe::makeShared<Tracked>(4);
        weak = owner;
        CHECK(!weak.expired());
    }
    // The object is gone, but the control block is kept alive by the weak reference
    CHECK_EQ(Tracked::alive.load(), 0);
    CHECK(weak.expired());
    CHECK(!weak.lock());
}

TEST(WeakPtr, doesNotKeepTheObjectAlive)
{
    Tracked::resetCounters();
    auto owner = safe::makeShared<Tracked>(4);
    safe::WeakPtr<Tracked> weak = owner;

    owner = safe::SharedPtr<Tracked>{};
    CHECK_EQ(Tracked::alive.load(), 0);
    CHECK(weak.expired());
}

TEST(WeakPtr, copyAndMoveShareTheSameObject)
{
    auto owner = safe::makeShared<Tracked>(4);
    safe::WeakPtr<Tracked> weak = owner;

    safe::WeakPtr<Tracked> copy = weak;
    CHECK_EQ(weakCount(owner), size_t(3));
    CHECK_EQ(copy.lock().readLock()->value, 4);

    safe::WeakPtr<Tracked> moved = std::move(copy);
    CHECK_EQ(weakCount(owner), size_t(3)); // moving transfers, it does not add
    CHECK_EQ(moved.lock().readLock()->value, 4);

    safe::WeakPtr<Tracked> assigned;
    assigned = moved;
    CHECK_EQ(weakCount(owner), size_t(4));
    CHECK_EQ(assigned.lock().readLock()->value, 4);
}

TEST(WeakPtr, reassigningReleasesThePreviousReference)
{
    auto first = safe::makeShared<Tracked>(1);
    auto second = safe::makeShared<Tracked>(2);

    safe::WeakPtr<Tracked> weak = first;
    CHECK_EQ(weakCount(first), size_t(2));

    weak = second;
    CHECK_EQ(weakCount(first), size_t(1));
    CHECK_EQ(weakCount(second), size_t(2));
    CHECK_EQ(weak.lock().readLock()->value, 2);
}

TEST(WeakPtr, upCastsToABaseClass)
{
    auto owner = safe::makeShared<Derived>();

    safe::WeakPtr<Base> weakBase = owner;
    auto locked = weakBase.lock();
    REQUIRE(bool(locked));
    CHECK_EQ(locked.readLock()->base, 1);

    safe::WeakPtr<Base> fromWeak = safe::WeakPtr<Derived>(owner);
    CHECK_EQ(fromWeak.lock().readLock()->base, 1);
}

TEST(WeakPtr, upCastAdjustsThePointerWithMultipleInheritance)
{
    auto owner = safe::makeShared<Both>();

    safe::WeakPtr<Right> weakRight = owner;
    auto locked = weakRight.lock();
    REQUIRE(bool(locked));
    CHECK_EQ(locked.readLock()->right, 20);

    safe::WeakPtr<Left> weakLeft = owner;
    CHECK_EQ(weakLeft.lock().readLock()->left, 10);
}

TEST(WeakPtr, releasesTheControlBlockOnDestruction)
{
    auto owner = safe::makeShared<Tracked>(1);
    {
        safe::WeakPtr<Tracked> weak = owner;
        CHECK_EQ(weakCount(owner), size_t(2));
    }
    CHECK_EQ(weakCount(owner), size_t(1));
}
