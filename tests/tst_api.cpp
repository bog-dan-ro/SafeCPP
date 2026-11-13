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

#include <map>
#include <utility>

using namespace tst;

namespace {

struct Node : safe::EnableSharedFromThis<Node>
{
    int value = 0;

    explicit Node(int value = 0)
        : value(value)
    {}
};

// The back reference has to survive inheritance: makeShared() is handed the most derived
// type, while EnableSharedFromThis sits on a base and is parameterised on that base.
struct DerivedNode : Node
{
    int extra = 0;

    explicit DerivedNode(int value = 0, int extra = 0)
        : Node(value)
        , extra(extra)
    {}
};

struct GrandChildNode : DerivedNode
{
    using DerivedNode::DerivedNode;
};

struct PolymorphicNode : safe::EnableSharedFromThis<PolymorphicNode>
{
    virtual ~PolymorphicNode() = default;
    int value = 0;

    explicit PolymorphicNode(int value = 0)
        : value(value)
    {}
};

struct DerivedPolymorphicNode : PolymorphicNode
{
    int extra = 0;

    explicit DerivedPolymorphicNode(int value = 0, int extra = 0)
        : PolymorphicNode(value)
        , extra(extra)
    {}
};

struct DerivedEsftOnly : Base, safe::EnableSharedFromThis<DerivedEsftOnly>
{
    int extra = 9;
};

// Declared with a non default mutex
struct TimedNode : safe::EnableSharedFromThis<TimedNode, std::shared_timed_mutex>
{
    int value = 0;
};

struct DerivedTimedNode : TimedNode
{};

// No back reference at all, must still be constructible
struct PlainNode
{
    int value = 0;
};

// Inaccessible base, treated as if there were no base at all
struct PrivateNode : private safe::EnableSharedFromThis<PrivateNode>
{
    int value = 0;
};

// Two back references, ambiguous, so neither is armed
struct AmbiguousNode : safe::EnableSharedFromThis<Node>, safe::EnableSharedFromThis<PlainNode>
{
    int value = 0;
};

struct ConstexprBase
{
    virtual constexpr ~ConstexprBase() = default;
};

struct ConstexprDerived : ConstexprBase
{};

struct ConstexprNode : safe::EnableSharedFromThis<ConstexprNode>
{};

constexpr bool emptySharedPointerApi()
{
    safe::SharedPtr<int> first;
    safe::SharedPtr<int> second{nullptr};
    safe::SharedPtr<int> copy(first);
    safe::SharedPtr<int> moved(std::move(copy));

    second = first;
    moved = std::move(second);
    first = nullptr;
    first.reset();
    swap(first, moved);

    safe::SharedPtr<const int> readOnly = first;
    auto writable = safe::constCast<int>(readOnly);
    safe::SharedPtr<ConstexprBase> base;
    auto derived = safe::staticCast<ConstexprDerived>(base);
    auto dynamic = safe::dynamicCast<ConstexprDerived>(base);

    return !first && !second && !copy && !moved && !readOnly && !writable && !derived
           && !dynamic && safe::getDeleter<int>(first) == nullptr
           && first.owner() == nullptr && first.useCount() == 0 && first.weakCount() == 0
           && (first <=> moved) == std::strong_ordering::equal;
}

constexpr bool emptyWeakPointerApi()
{
    safe::SharedPtr<int> shared;
    safe::WeakPtr<int> first(shared);
    safe::WeakPtr<int> copy(first);
    safe::WeakPtr<int> moved(std::move(copy));

    copy = first;
    moved = std::move(copy);
    first.reset();
    swap(first, moved);

    auto locked = moved.lock();
    return first.expired() && moved.expired() && !locked && first.owner() == nullptr
           && first.useCount() == 0 && (first <=> moved) == std::strong_ordering::equal;
}

constexpr bool emptyCopyOnWriteAndSharedFromThisApi()
{
    safe::CopyOnWritePtr<int> copyOnWrite;
    safe::CopyOnWritePtr<int> fromNull{nullptr};
    safe::CopyOnWritePtr<ConstexprDerived> derivedCow;
    safe::CopyOnWritePtr<ConstexprBase> baseCow(derivedCow);
    safe::CopyOnWritePtr<ConstexprBase> movedBaseCow(std::move(derivedCow));
    baseCow = movedBaseCow;
    baseCow = std::move(movedBaseCow);
    baseCow = nullptr;

    ConstexprNode node;
    return !copyOnWrite && !fromNull && !derivedCow && !baseCow && !movedBaseCow
           && !node.sharedFromThis() && node.weakFromThis().expired();
}

static_assert(emptySharedPointerApi());
static_assert(emptyWeakPointerApi());
static_assert(emptyCopyOnWriteAndSharedFromThisApi());

} // namespace

TEST(Api, exposesElementAndMutexTypes)
{
    static_assert(std::is_same_v<safe::SharedPtr<Tracked>::ElementType, Tracked>);
    static_assert(std::is_same_v<safe::SharedPtr<Tracked>::MutexType, std::shared_mutex>);
    static_assert(std::is_same_v<safe::WeakPtr<Tracked>::ElementType, Tracked>);
    CHECK(true);
}

TEST(Api, constructsAndComparesAgainstNullptr)
{
    safe::SharedPtr<Tracked> empty = nullptr;
    CHECK(empty == nullptr);
    CHECK(!empty);

    auto pointer = safe::makeShared<Tracked>(1);
    CHECK(!(pointer == nullptr));

    pointer = nullptr;
    CHECK(pointer == nullptr);
}

TEST(Api, resetReleasesTheObject)
{
    Tracked::resetCounters();
    auto pointer = safe::makeShared<Tracked>(1);
    auto second = pointer;

    pointer.reset();
    CHECK(!pointer);
    CHECK_EQ(Tracked::alive.load(), 1);
    CHECK_EQ(second.useCount(), size_t(1));

    second.reset();
    CHECK_EQ(Tracked::alive.load(), 0);
    second.reset(); // resetting an empty pointer is a no-op
}

TEST(Api, swapsOwnership)
{
    auto first = safe::makeShared<Tracked>(1);
    auto second = safe::makeShared<Tracked>(2);
    const void *firstOwner = first.owner();

    first.swap(second);
    CHECK_EQ(first.readLock()->value, 2);
    CHECK_EQ(second.readLock()->value, 1);
    CHECK(second.owner() == firstOwner);

    swap(first, second); // free function
    CHECK_EQ(first.readLock()->value, 1);
    CHECK_EQ(first.useCount(), size_t(1));
}

TEST(Api, comparesByOwnership)
{
    auto derived = safe::makeShared<Derived>();
    safe::SharedPtr<Base> base = derived;
    auto other = safe::makeShared<Derived>();

    CHECK(base == derived); // same object seen through different base classes
    CHECK(!(base == other));
    CHECK((base <=> derived) == std::strong_ordering::equal);
    CHECK((base <=> other) != std::strong_ordering::equal);

    // usable as an ordered key
    std::map<safe::SharedPtr<Derived>, int> byPointer;
    byPointer[derived] = 1;
    byPointer[other] = 2;
    CHECK_EQ(byPointer.size(), size_t(2));
    CHECK_EQ(byPointer[derived], 1);
}

TEST(Api, constCastRemovesConstness)
{
    auto pointer = safe::makeShared<Tracked>(1);
    safe::SharedPtr<const Tracked> readOnly = pointer;

    auto writable = safe::constCast<Tracked>(readOnly);
    REQUIRE(bool(writable));
    CHECK(writable == pointer);
    writable.writeLock()->value = 7;
    CHECK_EQ(readOnly.readLock()->value, 7);
}

TEST(Api, weakPointerMoveAssignmentTransfersTheReference)
{
    auto owner = safe::makeShared<Tracked>(1);
    safe::WeakPtr<Tracked> first = owner;
    safe::WeakPtr<Tracked> second;

    second = std::move(first);
    CHECK_EQ(owner.weakCount(), size_t(2)); // moved, not duplicated
    CHECK(first.expired());
    CHECK_EQ(second.lock().readLock()->value, 1);
}

TEST(Api, weakPointerResetSwapAndUseCount)
{
    auto owner = safe::makeShared<Tracked>(1);
    safe::WeakPtr<Tracked> weak = owner;
    CHECK_EQ(weak.useCount(), size_t(1));

    auto second = owner;
    CHECK_EQ(weak.useCount(), size_t(2));

    safe::WeakPtr<Tracked> other;
    weak.swap(other);
    CHECK(weak.expired());
    CHECK_EQ(other.useCount(), size_t(2));

    other.reset();
    CHECK(other.expired());
    CHECK_EQ(owner.weakCount(), size_t(1));
}

TEST(Api, sharedFromThisReturnsAnOwningPointer)
{
    auto node = safe::makeShared<Node>(5);

    auto self = node.readLock()->sharedFromThis();
    REQUIRE(bool(self));
    CHECK(self == node);
    CHECK_EQ(self.useCount(), size_t(2)); // node and self, the guard has gone
    CHECK_EQ(self.readLock()->value, 5);

    auto weak = node.readLock()->weakFromThis();
    CHECK(!weak.expired());
}

TEST(Api, sharedFromThisFollowsACopyOnWriteDetach)
{
    auto original = safe::makeCopyOnWrite<Node>(1);
    auto copy = original;

    copy.writeLock()->value = 2; // detaches onto a fresh block

    auto self = copy.readLock()->sharedFromThis();
    REQUIRE(bool(self));
    CHECK(self == copy); // the clone points at itself, not at the original
    CHECK(!(self == original));
    CHECK_EQ(self.readLock()->value, 2);

    auto originalSelf = original.readLock()->sharedFromThis();
    CHECK(originalSelf == original);
}

TEST(Api, sharedFromThisIsEmptyForAnUnownedObject)
{
    Node stackNode(1);
    CHECK(!stackNode.sharedFromThis());
}

TEST(Api, sharedFromThisIsArmedForADerivedType)
{
    auto node = safe::makeShared<DerivedNode>(5, 7);

    auto self = node.readLock()->sharedFromThis(); // safe::SharedPtr<Node>, the base
    REQUIRE(bool(self));
    CHECK(self == node); // same control block, so the two compare equal
    CHECK_EQ(self.readLock()->value, 5);
    CHECK_EQ(self.useCount(), size_t(2)); // node and self, the guard has gone

    auto weak = node.readLock()->weakFromThis();
    CHECK(!weak.expired());
    CHECK(weak.lock() == node);
}

TEST(Api, sharedFromThisIsArmedThroughAnIndirectBase)
{
    auto node = safe::makeShared<GrandChildNode>(3, 4);

    auto self = node.readLock()->sharedFromThis();
    REQUIRE(bool(self));
    CHECK(self == node);
    CHECK_EQ(self.readLock()->value, 3);
}

TEST(Api, sharedFromThisDoesNotAddAnOwnerOfItsOwn)
{
    // The back reference is weak, so an armed object must not keep itself alive
    auto node = safe::makeShared<DerivedNode>(1, 2);
    CHECK_EQ(node.useCount(), size_t(1));
    CHECK_EQ(node.weakCount(), size_t(2)); // the owners collectively, plus m_weakThis

    safe::WeakPtr<Node> observer = node.readLock()->weakFromThis();
    CHECK(!observer.expired());
    node.reset();
    CHECK(observer.expired());
}

TEST(Api, sharedFromThisIsArmedForADerivedTypeWithANonDefaultMutex)
{
    auto node = safe::makeShared<DerivedTimedNode, std::shared_timed_mutex>();

    auto self = node.readLock()->sharedFromThis();
    REQUIRE(bool(self));
    CHECK(self == node);
}

TEST(Api, sharedFromThisIsNotArmedWhenTheMutexDoesNotMatch)
{
    // The object asks for shared_timed_mutex, the pointer uses the default one, so this
    // is not the base we are looking for and the back reference stays empty
    auto node = safe::makeShared<DerivedTimedNode>();
    CHECK(!node.readLock()->sharedFromThis());
}

TEST(Api, objectsWithoutABackReferenceStillWork)
{
    auto plain = safe::makeShared<PlainNode>();
    plain.writeLock()->value = 9;
    CHECK_EQ(plain.readLock()->value, 9);

    // An inaccessible or ambiguous base is simply not armed, exactly like the standard
    // enable_shared_from_this, and must not make makeShared() ill formed
    auto hidden = safe::makeShared<PrivateNode>();
    hidden.writeLock()->value = 8;
    CHECK_EQ(hidden.readLock()->value, 8);

    auto ambiguous = safe::makeShared<AmbiguousNode>();
    ambiguous.writeLock()->value = 7;
    CHECK_EQ(ambiguous.readLock()->value, 7);
}

TEST(Api, sharedFromThisFollowsACopyOnWriteDetachOfADerivedType)
{
    auto original = safe::makeCopyOnWrite<DerivedNode>(1, 2);
    auto copy = original;

    copy.writeLock()->value = 3; // detaches onto a fresh block

    auto self = copy.readLock()->sharedFromThis();
    REQUIRE(bool(self));
    CHECK(self == copy); // the clone points at itself, not at the original
    CHECK(!(self == original));
    CHECK_EQ(self.readLock()->value, 3);

    CHECK(original.readLock()->sharedFromThis() == original);
}

TEST(Api, sharedFromThisFollowsADetachThroughABaseCopyOnWritePointer)
{
    safe::CopyOnWritePtr<PolymorphicNode> d1 =
        safe::makeCopyOnWrite<DerivedPolymorphicNode>(1, 2);
    auto d2 = d1;

    {
        auto lock1 = d1.readLock();
        auto lock2 = d2.writeLock(); // detaches and clones DerivedPolymorphicNode
        lock2->value = 3;
    }

    auto self1 = d1.readLock()->sharedFromThis();
    auto self2 = d2.readLock()->sharedFromThis();
    REQUIRE(bool(self1));
    REQUIRE(bool(self2));
    CHECK(self1 == d1);
    CHECK(self2 == d2);
    CHECK(!(self1 == self2));

    auto derived2 = safe::dynamicCast<DerivedPolymorphicNode>(self2);
    REQUIRE(bool(derived2));
    CHECK(derived2 == d2);
    CHECK_EQ(derived2.readLock()->value, 3);
    CHECK_EQ(derived2.readLock()->extra, 2);
}

TEST(Api, sharedFromThisOnADerivedTypeSurvivesDetachThroughAnUnrelatedBase)
{
    safe::CopyOnWritePtr<Base> d1 = safe::makeCopyOnWrite<DerivedEsftOnly>();
    auto d2 = d1;

    d2.writeLock()->base = 42;

    auto derived2 = safe::dynamicCast<DerivedEsftOnly>(d2);
    REQUIRE(bool(derived2));
    auto self2 = derived2.readLock()->sharedFromThis();
    REQUIRE(bool(self2));
    CHECK(self2 == d2);
    CHECK(!(self2 == d1));
    CHECK_EQ(self2.readLock()->base, 42);
    CHECK_EQ(self2.readLock()->extra, 9);
}

TEST(ScopedLock, locksSeveralPointersAtOnce)
{
    auto first = safe::makeShared<Tracked>(1);
    auto second = safe::makeShared<Tracked>(2);

    {
        auto guard = safe::lockAll(first, second);
        guard.get<0>().value = 10;
        guard.get<1>().value = 20;
        CHECK_EQ(first.useCount(), size_t(2)); // the pointer plus the lock
        CHECK_EQ(second.useCount(), size_t(2));
    }

    CHECK_EQ(first.readLock()->value, 10);
    CHECK_EQ(second.readLock()->value, 20);
    CHECK_EQ(first.useCount(), size_t(1));
}

TEST(ScopedLock, argumentOrderDoesNotMatter)
{
    auto first = safe::makeShared<Tracked>(1);
    auto second = safe::makeShared<Tracked>(2);

    {
        auto guard = safe::lockAll(second, first);
        guard.get<0>().value = 20;
        guard.get<1>().value = 10;
    }
    CHECK_EQ(first.readLock()->value, 10);
    CHECK_EQ(second.readLock()->value, 20);
}

TEST(ScopedLock, theSamePointerTwiceDoesNotSelfDeadlock)
{
    auto pointer = safe::makeShared<Tracked>(1);

    {
        auto guard = safe::lockAll(pointer, pointer);
        guard.get<0>().value = 5;
        CHECK_EQ(guard.get<1>().value, 5); // both views are the same object
        CHECK_EQ(pointer.useCount(), size_t(2)); // locked once, so one reference
    }
    CHECK_EQ(pointer.readLock()->value, 5);
    CHECK_EQ(pointer.useCount(), size_t(1));
}

TEST(ScopedLock, worksWithMixedElementTypesAndKeepsObjectsAlive)
{
    Tracked::resetCounters();
    auto tracked = safe::makeShared<Tracked>(1);
    auto derived = safe::makeShared<Derived>();

    {
        auto guard = safe::lockAll(tracked, derived);
        guard.get<0>().value = 3;
        guard.get<1>().derived = 4;

        tracked.reset(); // the lock is now the only owner
        CHECK_EQ(Tracked::alive.load(), 1);
        CHECK_EQ(guard.get<0>().value, 3);
    }
    CHECK_EQ(Tracked::alive.load(), 0);
    CHECK_EQ(derived.readLock()->derived, 4);
}
