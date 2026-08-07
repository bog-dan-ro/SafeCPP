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

#include <utility>

using namespace tst;

namespace {

void takesABasePointer(const safe::SharedPtr<Base> &pointer, int expectedBase)
{
    CHECK_EQ(pointer.readLock()->base, expectedBase);
}

} // namespace

TEST(Cast, derivedConvertsToBaseImplicitly)
{
    auto derived = safe::makeShared<Derived>();

    safe::SharedPtr<Base> base = derived;
    CHECK_EQ(sharedCount(derived), size_t(2));
    CHECK(base.owner() == derived.owner());
    CHECK_EQ(base.readLock()->base, 1);

    takesABasePointer(derived, 1); // implicit conversion at the call site
}

TEST(Cast, copyOnWritePointerConvertsToASharedBasePointer)
{
    auto derived = safe::makeCopyOnWrite<Derived>();
    takesABasePointer(derived, 1);

    safe::SharedPtr<Base> base = derived;
    CHECK(base.owner() == derived.owner());
    CHECK_EQ(base.readLock()->base, 1);
}

TEST(Cast, convertingMoveLeavesTheSourceEmpty)
{
    auto derived = safe::makeShared<Derived>();
    const void *controlBlock = derived.owner();

    safe::SharedPtr<Base> base = std::move(derived);
    CHECK(!derived);
    REQUIRE(bool(base));
    CHECK(base.owner() == controlBlock);
    CHECK_EQ(sharedCount(base), size_t(1));
}

TEST(Cast, convertingAssignmentSharesOwnership)
{
    auto derived = safe::makeShared<Derived>();
    safe::SharedPtr<Base> base;

    base = derived;
    CHECK_EQ(sharedCount(derived), size_t(2));
    CHECK_EQ(base.readLock()->base, 1);

    auto other = safe::makeShared<Derived>();
    base = std::move(other);
    CHECK(!other);
    CHECK_EQ(sharedCount(derived), size_t(1));
    CHECK_EQ(sharedCount(base), size_t(1));
}

TEST(Cast, convertsToAConstPointee)
{
    auto pointer = safe::makeShared<Tracked>(9);
    safe::SharedPtr<const Tracked> readOnly = pointer;

    CHECK_EQ(sharedCount(pointer), size_t(2));
    CHECK_EQ(readOnly.readLock()->value, 9);
    CHECK_EQ(readOnly.writeLock()->value, 9); // still const, T is const Tracked
}

TEST(Cast, staticCastGoesBackDownTheHierarchy)
{
    auto derived = safe::makeShared<Derived>();
    safe::SharedPtr<Base> base = derived;

    auto back = safe::staticCast<Derived>(base);
    REQUIRE(bool(back));
    CHECK(back.owner() == derived.owner());
    CHECK_EQ(back.readLock()->derived, 2);
    CHECK_EQ(sharedCount(derived), size_t(3));
}

TEST(Cast, dynamicCastGoesBackDownTheHierarchy)
{
    auto derived = safe::makeShared<Derived>();
    safe::SharedPtr<Base> base = derived;

    auto back = safe::dynamicCast<Derived>(base);
    REQUIRE(bool(back));
    CHECK(back.owner() == derived.owner());
    CHECK_EQ(back.readLock()->derived, 2);
    CHECK_EQ(sharedCount(derived), size_t(3));
}

TEST(Cast, failingDynamicCastYieldsAnEmptyPointerAndTakesNoReference)
{
    safe::SharedPtr<Base> base = safe::makeShared<Unrelated>();

    auto wrong = safe::dynamicCast<Derived>(base);
    CHECK(!wrong);
    CHECK(wrong.owner() == nullptr);
    CHECK_EQ(sharedCount(base), size_t(1));

    auto right = safe::dynamicCast<Unrelated>(base);
    REQUIRE(bool(right));
    CHECK_EQ(right.readLock()->unrelated, 3);
}

TEST(Cast, castingAnEmptyPointerStaysEmpty)
{
    safe::SharedPtr<Base> empty;
    CHECK(!safe::staticCast<Derived>(empty));
    CHECK(!safe::dynamicCast<Derived>(empty));
}

TEST(Cast, adjustsTheAddressWithMultipleInheritance)
{
    auto both = safe::makeShared<Both>();

    safe::SharedPtr<Right> right = both;
    CHECK_EQ(right.readLock()->right, 20);
    safe::SharedPtr<Left> left = both;
    CHECK_EQ(left.readLock()->left, 10);

    // All three share one control block while pointing at different addresses
    CHECK(right.owner() == both.owner());
    CHECK(left.owner() == both.owner());

    auto backFromRight = safe::staticCast<Both>(right);
    CHECK_EQ(backFromRight.readLock()->both, 30);
    CHECK_EQ(backFromRight.readLock()->left, 10);

    auto backFromLeft = safe::dynamicCast<Both>(left);
    REQUIRE(bool(backFromLeft));
    CHECK_EQ(backFromLeft.readLock()->right, 20);
}

TEST(Cast, dynamicCastCrossCastsBetweenSiblingBases)
{
    auto both = safe::makeShared<Both>();
    safe::SharedPtr<Left> left = both;

    auto right = safe::dynamicCast<Right>(left);
    REQUIRE(bool(right));
    CHECK_EQ(right.readLock()->right, 20);
    CHECK(right.owner() == both.owner());
}

TEST(Cast, castResultsKeepTheObjectAlive)
{
    safe::SharedPtr<Derived> castResult;
    {
        auto derived = safe::makeShared<Derived>();
        safe::SharedPtr<Base> base = derived;
        castResult = safe::dynamicCast<Derived>(base);
    }
    REQUIRE(bool(castResult));
    CHECK_EQ(sharedCount(castResult), size_t(1));
    CHECK_EQ(castResult.readLock()->derived, 2);
}

TEST(Cast, mutationThroughABaseViewIsVisibleToTheDerivedPointer)
{
    auto derived = safe::makeShared<Derived>();
    safe::SharedPtr<Base> base = derived;

    base.writeLock()->base = 77;
    CHECK_EQ(derived.readLock()->base, 77);

    derived.writeLock()->derived = 88;
    CHECK_EQ(safe::staticCast<Derived>(base).readLock()->derived, 88);
}

TEST(Cast, usesTheMutexOfTheOriginalControlBlock)
{
    InstrumentedMutex::resetCounters();
    auto derived = safe::makeShared<Derived, InstrumentedMutex>();
    safe::SharedPtr<Base, InstrumentedMutex> base = derived;

    {
        auto guard = base.readLock();
        CHECK_EQ(InstrumentedMutex::sharedLocks.load(), 1);
        CHECK_EQ(guard->base, 1);
    }
    CHECK_EQ(InstrumentedMutex::sharedUnlocks.load(), 1);

    auto back = safe::dynamicCast<Derived>(base);
    REQUIRE(bool(back));
    {
        auto guard = back.writeLock();
        CHECK_EQ(InstrumentedMutex::locks.load(), 1);
    }
    CHECK_EQ(InstrumentedMutex::unlocks.load(), 1);
}
