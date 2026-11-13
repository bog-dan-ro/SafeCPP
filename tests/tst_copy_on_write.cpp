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

#include <concepts>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

using namespace tst;

namespace {

struct AbstractBase
{
    virtual ~AbstractBase() = default;
    virtual int total() const = 0;
    int base = 5;
};

struct ConcreteDerived : AbstractBase
{
    int extra = 10;
    int total() const override { return base + extra; }
};

struct NonCopyable
{
    NonCopyable() = default;
    NonCopyable(const NonCopyable &) = delete;
    int value = 1;
};

// Keeps one int inside the object and one outside of it
struct Holder
{
    std::unique_ptr<int> remote = std::make_unique<int>(5);
    int local = 6;

    Holder() = default;
    Holder(const Holder &other)
        : remote(std::make_unique<int>(*other.remote))
        , local(other.local)
    {}
};

template<typename T>
concept CanMakeCopyOnWrite = requires { safe::makeCopyOnWrite<T>(); };

template<typename T>
concept CanAllocateCopyOnWrite =
    requires { safe::allocateCopyOnWrite<T>(std::allocator<std::byte>{}); };

template<typename P, typename Arg>
concept CanResetTo = requires(P pointer, Arg argument) { pointer.reset(argument); };

template<typename P, typename Other>
concept CanSwapWith = requires(P pointer, Other other) { pointer.swap(other); };

template<typename P>
concept CanWriteLock = requires(P pointer) { pointer.writeLock(); };

// A type that cannot be copied is rejected where the pointer is created, never at
// the first detach
static_assert(CanMakeCopyOnWrite<Tracked>);
static_assert(!CanMakeCopyOnWrite<NonCopyable>);
static_assert(!CanMakeCopyOnWrite<AbstractBase>);
static_assert(CanAllocateCopyOnWrite<Tracked>);
static_assert(!CanAllocateCopyOnWrite<NonCopyable>);

static_assert(std::is_constructible_v<safe::CopyOnWritePtr<Tracked>, safe::SharedPtr<Tracked>>);
static_assert(
    !std::is_constructible_v<safe::CopyOnWritePtr<NonCopyable>, safe::SharedPtr<NonCopyable>>);
static_assert(!std::is_constructible_v<safe::CopyOnWritePtr<NonCopyable>,
                                       const safe::SharedPtr<NonCopyable> &>);
static_assert(
    !std::is_assignable_v<safe::CopyOnWritePtr<NonCopyable> &, safe::SharedPtr<NonCopyable>>);

// A SharedPtr<AbstractBase> does not know what to clone, a CopyOnWritePtr to the
// concrete type does and keeps knowing it after the up-cast
static_assert(
    !std::is_constructible_v<safe::CopyOnWritePtr<AbstractBase>, safe::SharedPtr<AbstractBase>>);
static_assert(
    !std::is_assignable_v<safe::CopyOnWritePtr<AbstractBase> &, safe::SharedPtr<AbstractBase>>);
static_assert(std::is_constructible_v<safe::CopyOnWritePtr<AbstractBase>,
                                      safe::SharedPtr<ConcreteDerived>>);
static_assert(std::is_convertible_v<safe::CopyOnWritePtr<ConcreteDerived>,
                                    safe::CopyOnWritePtr<AbstractBase>>);
static_assert(std::is_convertible_v<const safe::CopyOnWritePtr<Derived> &,
                                    safe::CopyOnWritePtr<Base>>);

// ... but never the other way around
static_assert(
    !std::is_convertible_v<safe::CopyOnWritePtr<Base>, safe::CopyOnWritePtr<Derived>>);
static_assert(!std::is_constructible_v<safe::CopyOnWritePtr<Derived>, safe::SharedPtr<Base>>);

// The SharedPtr ways of adopting a raw pointer are not inherited
static_assert(!std::is_constructible_v<safe::CopyOnWritePtr<Tracked>, Tracked *>);
static_assert(!CanResetTo<safe::CopyOnWritePtr<Tracked>, Tracked *>);
static_assert(CanResetTo<safe::SharedPtr<Tracked>, Tracked *>);
static_assert(!CanSwapWith<safe::CopyOnWritePtr<Tracked>, safe::SharedPtr<Tracked>>);
static_assert(CanSwapWith<safe::CopyOnWritePtr<Tracked>, safe::CopyOnWritePtr<Tracked>>);

// Writing through a const CopyOnWritePtr could not detach, so it does not compile
static_assert(CanWriteLock<safe::CopyOnWritePtr<Tracked> &>);
static_assert(!CanWriteLock<const safe::CopyOnWritePtr<Tracked> &>);

int readBase(const safe::CopyOnWritePtr<Base> &pointer)
{
    return pointer.readLock()->base;
}

safe::CopyOnWritePtr<Base> writeBase(safe::CopyOnWritePtr<Base> pointer, int value)
{
    pointer.writeLock()->base = value;
    return pointer;
}

} // namespace

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

TEST(CopyOnWrite, detachingABasePointerClonesTheDerivedObject)
{
    safe::CopyOnWritePtr<Base> d1 = safe::makeCopyOnWrite<Derived>();
    auto d2 = d1;

    {
        auto lock1 = d1.readLock();
        auto lock2 = d2.writeLock(); // detaches while lock1 holds a read lock on d1
        lock2->base = 99;
    }

    CHECK(d2.owner() != d1.owner());
    CHECK_EQ(d1.readLock()->base, 1);

    auto derived = safe::dynamicCast<Derived>(d2);
    REQUIRE(bool(derived));
    CHECK(derived.owner() == d2.owner());
    CHECK_EQ(derived.readLock()->base, 99);
    CHECK_EQ(derived.readLock()->derived, 2);
}

TEST(CopyOnWrite, detachesThroughAnAbstractBasePointer)
{
    safe::CopyOnWritePtr<AbstractBase> d1 = safe::makeCopyOnWrite<ConcreteDerived>();
    auto d2 = d1;

    d2.writeLock()->base = 20;

    CHECK(d2.owner() != d1.owner());
    CHECK_EQ(d1.readLock()->total(), 15);
    CHECK_EQ(d2.readLock()->total(), 30);

    auto derived = safe::dynamicCast<ConcreteDerived>(d2);
    REQUIRE(bool(derived));
    CHECK_EQ(derived.readLock()->extra, 10);
}

TEST(CopyOnWrite, detachingWithMultipleInheritanceAdjustsTheSubobjectPointer)
{
    safe::CopyOnWritePtr<Right> r1 = safe::makeCopyOnWrite<Both>();
    auto r2 = r1;

    r2.writeLock()->right = 200;

    CHECK(r2.owner() != r1.owner());
    CHECK_EQ(r1.readLock()->right, 20);
    CHECK_EQ(r2.readLock()->right, 200);

    auto both = safe::dynamicCast<Both>(r2);
    REQUIRE(bool(both));
    CHECK(both.owner() == r2.owner());
    CHECK_EQ(both.readLock()->left, 10);
    CHECK_EQ(both.readLock()->right, 200);
    CHECK_EQ(both.readLock()->both, 30);

    auto left = safe::dynamicCast<Left>(r2);
    REQUIRE(bool(left));
    CHECK_EQ(left.readLock()->left, 10);
}

TEST(CopyOnWrite, supportsConvertingConstructorsAndAssignments)
{
    auto derivedCow = safe::makeCopyOnWrite<Derived>();
    safe::CopyOnWritePtr<Base> fromCowCopy(derivedCow);
    CHECK(fromCowCopy.owner() == derivedCow.owner());

    safe::CopyOnWritePtr<Base> fromCowMove(std::move(derivedCow));
    CHECK(!derivedCow);
    REQUIRE(bool(fromCowMove));

    auto derivedShared = safe::makeShared<Derived>();
    safe::CopyOnWritePtr<Base> fromSharedCopy(derivedShared);
    CHECK(fromSharedCopy.owner() == derivedShared.owner());

    safe::CopyOnWritePtr<Base> fromSharedMove(std::move(derivedShared));
    CHECK(!derivedShared);
    REQUIRE(bool(fromSharedMove));

    safe::CopyOnWritePtr<Base> assigned = nullptr;
    CHECK(!assigned);

    auto anotherCow = safe::makeCopyOnWrite<Derived>();
    assigned = anotherCow;
    CHECK(assigned.owner() == anotherCow.owner());

    assigned = nullptr;
    CHECK(!assigned);

    assigned = std::move(anotherCow);
    CHECK(!anotherCow);
    REQUIRE(bool(assigned));

    auto anotherShared = safe::makeShared<Derived>();
    assigned = anotherShared;
    CHECK(assigned.owner() == anotherShared.owner());

    auto movedShared = safe::makeShared<Derived>();
    const void *movedOwner = movedShared.owner();
    assigned = std::move(movedShared);
    CHECK(!movedShared);
    CHECK(assigned.owner() == movedOwner);
}

TEST(CopyOnWrite, upcastsImplicitlyWhenPassedToAFunction)
{
    auto derived = safe::makeCopyOnWrite<Derived>();
    derived.writeLock()->derived = 7;

    CHECK_EQ(readBase(derived), 1); // no cast needed
    CHECK_EQ(sharedCount(derived), size_t(1)); // the temporary is gone again

    // By value: the callee writes, so it detaches and returns a clone of the
    // complete Derived object
    auto written = writeBase(derived, 50);
    CHECK(written.owner() != derived.owner());
    CHECK_EQ(derived.readLock()->base, 1);

    auto writtenDerived = safe::dynamicCast<Derived>(written);
    REQUIRE(bool(writtenDerived));
    CHECK_EQ(writtenDerived.readLock()->base, 50);
    CHECK_EQ(writtenDerived.readLock()->derived, 7);

    // An rvalue is the sole owner, nothing to clone
    Tracked::resetCounters();
    auto moved = writeBase(safe::makeCopyOnWrite<Derived>(), 60);
    CHECK_EQ(moved.readLock()->base, 60);
    CHECK_EQ(sharedCount(moved), size_t(1));
}

TEST(CopyOnWrite, upcastOfAnAdoptedSharedPointerStillClonesTheCompleteObject)
{
    // Not created by makeCopyOnWrite(): the block cannot clone, the pointer
    // remembers the type it was adopted as and carries that through the up-cast
    auto shared = safe::makeShared<Both>();
    safe::CopyOnWritePtr<Both> both = shared;
    safe::CopyOnWritePtr<Right> right = both; // shifts the address
    both.reset();

    right.writeLock()->right = 200;

    CHECK(right.owner() != shared.owner());
    CHECK_EQ(shared.readLock()->right, 20);

    auto clone = safe::dynamicCast<Both>(right);
    REQUIRE(bool(clone));
    CHECK_EQ(clone.readLock()->left, 10);
    CHECK_EQ(clone.readLock()->right, 200);
    CHECK_EQ(clone.readLock()->both, 30);

    // Straight from the SharedPtr<Derived>, in one step
    auto sharedDerived = safe::makeShared<Derived>();
    safe::CopyOnWritePtr<Base> base = sharedDerived;
    base.writeLock()->base = 9;

    auto cloneDerived = safe::dynamicCast<Derived>(base);
    REQUIRE(bool(cloneDerived));
    CHECK(cloneDerived.owner() != sharedDerived.owner());
    CHECK_EQ(cloneDerived.readLock()->base, 9);
    CHECK_EQ(cloneDerived.readLock()->derived, 2);
    CHECK_EQ(sharedDerived.readLock()->base, 1);
}

TEST(CopyOnWrite, aCloneOfAnAdoptedObjectCanBeClonedAgain)
{
    auto shared = safe::makeShared<Derived>();
    safe::CopyOnWritePtr<Base> first = shared;
    first.writeLock()->base = 2; // first now owns a clone made by the fallback

    auto second = first;
    second.writeLock()->base = 3; // and that clone knows how to clone itself

    CHECK_EQ(shared.readLock()->base, 1);
    CHECK_EQ(first.readLock()->base, 2);
    CHECK_EQ(second.readLock()->base, 3);
    REQUIRE(bool(safe::dynamicCast<Derived>(second)));
}

TEST(CopyOnWrite, adoptingADowncastPointerClonesTheCompleteObject)
{
    // The block only ever saw a Base *, so it cannot be the one deciding what a
    // clone looks like: cloning a Base and handing it out as a Derived would read
    // and write past the end of it
    Base *raw = new Derived;
    safe::SharedPtr<Base> shared(raw);

    safe::CopyOnWritePtr<Derived> derived(safe::dynamicCast<Derived>(shared));
    REQUIRE(bool(derived));

    derived.writeLock()->derived = 77;

    CHECK(derived.owner() != shared.owner());
    CHECK_EQ(derived.readLock()->base, 1);
    CHECK_EQ(derived.readLock()->derived, 77);
    CHECK_EQ(safe::dynamicCast<Derived>(shared).readLock()->derived, 2);
}

TEST(CopyOnWrite, adoptingThroughABasePointerClonesTheStaticType)
{
    // The documented limit of adopting a plain SharedPtr: all there is to go by is
    // its static type. The result is a valid Base, never a half Derived.
    safe::SharedPtr<Base> shared(new Derived);
    safe::CopyOnWritePtr<Base> cow = shared;

    cow.writeLock()->base = 4;

    CHECK(cow.owner() != shared.owner());
    CHECK_EQ(cow.readLock()->base, 4);
    CHECK(!safe::dynamicCast<Derived>(cow));
    CHECK(bool(safe::dynamicCast<Derived>(shared)));
    CHECK_EQ(shared.readLock()->base, 1);
}

TEST(CopyOnWrite, detachingAnAliasToASubobjectClonesTheOwner)
{
    auto owner = safe::makeCopyOnWrite<Holder>();
    int *local = &owner.writeLock()->local;

    safe::CopyOnWritePtr<int> alias(safe::SharedPtr<int>(owner, local));
    CHECK(alias.owner() == owner.owner());

    *alias.writeLock() = 60;

    CHECK(alias.owner() != owner.owner());
    CHECK_EQ(owner.readLock()->local, 6);
    CHECK_EQ(*alias.readLock(), 60);
}

TEST(CopyOnWrite, detachingAnAliasOutsideOfTheOwnerCopiesTheAliasedValue)
{
    // remote lives in an allocation of its own. There is no such place inside a
    // clone of the Holder, so the int itself is what gets copied.
    {
        auto owner = safe::makeCopyOnWrite<Holder>(); // a block that can clone
        int *remote = owner.writeLock()->remote.get();

        safe::CopyOnWritePtr<int> alias(safe::SharedPtr<int>(owner, remote));
        *alias.writeLock() = 50;

        CHECK(alias.owner() != owner.owner());
        CHECK_EQ(*owner.readLock()->remote, 5);
        CHECK_EQ(*alias.readLock(), 50);
    }
    {
        auto owner = safe::makeShared<Holder>(); // a block that cannot
        int *remote = owner.writeLock()->remote.get();

        safe::CopyOnWritePtr<int> alias(safe::SharedPtr<int>(owner, remote));
        *alias.writeLock() = 50;

        CHECK(alias.owner() != owner.owner());
        CHECK_EQ(*owner.readLock()->remote, 5);
        CHECK_EQ(*alias.readLock(), 50);
    }
}

TEST(CopyOnWrite, swapAndAssignmentCarryTheCloneAlong)
{
    auto sharedDerived = safe::makeShared<Derived>();
    safe::CopyOnWritePtr<Base> adopted = sharedDerived;
    safe::CopyOnWritePtr<Base> made = safe::makeCopyOnWrite<Unrelated>();
    auto madeKeeper = made;

    adopted.swap(made);
    CHECK(made.owner() == sharedDerived.owner());
    CHECK(adopted.owner() == madeKeeper.owner());

    made.writeLock()->base = 2;
    adopted.writeLock()->base = 3;
    CHECK(bool(safe::dynamicCast<Derived>(made)));
    CHECK(bool(safe::dynamicCast<Unrelated>(adopted)));

    // Assigning replaces whatever the target knew about its previous object
    safe::CopyOnWritePtr<Base> target = safe::makeShared<Base>();
    target = sharedDerived;
    target.writeLock()->base = 5;
    CHECK(bool(safe::dynamicCast<Derived>(target)));
    CHECK_EQ(sharedDerived.readLock()->base, 1);

    target = madeKeeper;
    target.writeLock()->base = 6;
    CHECK(bool(safe::dynamicCast<Unrelated>(target)));
    CHECK_EQ(madeKeeper.readLock()->base, 1);
}

TEST(CopyOnWrite, refusesToWriteAfterBeingReseatedThroughASharedPointerReference)
{
    Tracked::resetCounters();
    auto plain = safe::makeShared<Tracked>(2);
    auto cow = safe::makeCopyOnWrite<Tracked>(1);

    // Bypasses CopyOnWritePtr: it now shares an object nobody told it how to clone
    safe::SharedPtr<Tracked> &asBase = cow;
    asBase = plain;

    bool refused = false;
    try {
        cow.writeLock()->value = 3;
    } catch (const std::logic_error &) {
        refused = true;
    }

    CHECK(refused);
    CHECK_EQ(plain.readLock()->value, 2); // the other owner was not written to
    CHECK(cow.owner() == plain.owner());
    CHECK_EQ(Tracked::copied.load(), 0);

    // Still usable: as the sole owner there is nothing to clone
    plain.reset();
    cow.writeLock()->value = 4;
    CHECK_EQ(cow.readLock()->value, 4);
}

TEST(CopyOnWrite, writeLockThroughAConstSharedPointerWritesInPlaceOnPurpose)
{
    // Documented in the README: a const SharedPtr & is the way to update the
    // shared object for every owner without detaching
    Tracked::resetCounters();
    auto a = safe::makeCopyOnWrite<Tracked>(1);
    auto b = a;

    const safe::SharedPtr<Tracked> &shared = a;
    shared.writeLock()->value = 5;

    CHECK(a.owner() == b.owner());
    CHECK_EQ(Tracked::copied.load(), 0);
    CHECK_EQ(b.readLock()->value, 5);

    a.writeLock()->value = 6; // the pointer itself still detaches
    CHECK(a.owner() != b.owner());
    CHECK_EQ(Tracked::copied.load(), 1);
    CHECK_EQ(b.readLock()->value, 5);
    CHECK_EQ(a.readLock()->value, 6);
}
