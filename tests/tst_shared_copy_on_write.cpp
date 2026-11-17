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

#include <compare>
#include <concepts>
#include <cstddef>
#include <latch>
#include <map>
#include <memory>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using namespace tst;

namespace {

// Deliberately not polymorphic: nothing but the block that created the object
// knows that a Value is really a DerivedValue
struct Value
{
    int val = 10;
};

struct DerivedValue : Value
{
    int other = 20;
};

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

struct ThrowsOnCopy
{
    static inline bool armed = false;
    int value = 1;

    ThrowsOnCopy() = default;
    ThrowsOnCopy(const ThrowsOnCopy &other)
        : value(other.value)
    {
        if (armed)
            throw std::runtime_error("copy refused");
    }
};

template<typename T>
concept CanMakeSharedCopyOnWrite = requires { safe::makeSharedCopyOnWrite<T>(); };

template<typename T>
concept CanAllocateSharedCopyOnWrite =
    requires { safe::allocateSharedCopyOnWrite<T>(std::allocator<std::byte>{}); };

template<typename P>
concept CanWriteLock = requires(P pointer) { pointer.writeLock(); };

template<typename P>
concept CanLockAll = requires(P pointer) { safe::lockAll(pointer); };

template<typename P, typename Target>
concept CanStaticCast = requires(const P &pointer) { safe::staticCast<Target>(pointer); };

template<typename P, typename Target>
concept CanDynamicCast = requires(const P &pointer) { safe::dynamicCast<Target>(pointer); };

template<typename P, typename Target>
concept CanConstCast = requires(const P &pointer) { safe::constCast<Target>(pointer); };

template<typename A, typename B>
concept ComparableBothWays = requires(const A &a, const B &b) {
    { a == b } -> std::same_as<bool>;
    { b == a } -> std::same_as<bool>;
    { a != b } -> std::same_as<bool>;
    { b != a } -> std::same_as<bool>;
};

static_assert(CanMakeSharedCopyOnWrite<Tracked>);
static_assert(!CanMakeSharedCopyOnWrite<NonCopyable>);
static_assert(!CanMakeSharedCopyOnWrite<AbstractBase>);
static_assert(CanAllocateSharedCopyOnWrite<Tracked>);
static_assert(!CanAllocateSharedCopyOnWrite<NonCopyable>);

// The whole point: a function returning a SharedCopyOnWritePtr can return an
// element of a container of CopyOnWritePtr as it is, const or not, also as one
// of its base classes
static_assert(std::is_convertible_v<safe::CopyOnWritePtr<Tracked> &,
                                    safe::SharedCopyOnWritePtr<Tracked>>);
static_assert(std::is_convertible_v<const safe::CopyOnWritePtr<Tracked> &,
                                    safe::SharedCopyOnWritePtr<Tracked>>);
static_assert(
    std::is_convertible_v<safe::CopyOnWritePtr<Tracked>, safe::SharedCopyOnWritePtr<Tracked>>);
static_assert(std::is_convertible_v<const safe::CopyOnWritePtr<Derived> &,
                                    safe::SharedCopyOnWritePtr<Base>>);
static_assert(std::is_convertible_v<const safe::CopyOnWritePtr<ConcreteDerived> &,
                                    safe::SharedCopyOnWritePtr<AbstractBase>>);
static_assert(!std::is_constructible_v<safe::SharedCopyOnWritePtr<Derived>,
                                       const safe::CopyOnWritePtr<Base> &>);
static_assert(std::is_assignable_v<safe::SharedCopyOnWritePtr<Tracked> &,
                                   const safe::CopyOnWritePtr<Tracked> &>);

// A plain SharedPtr has to say how it is cloned first, by becoming a CopyOnWritePtr
static_assert(
    !std::is_constructible_v<safe::SharedCopyOnWritePtr<Tracked>, safe::SharedPtr<Tracked>>);
static_assert(!std::is_constructible_v<safe::SharedCopyOnWritePtr<Tracked>, Tracked *>);

// Pointers to the same slot convert to a base, never back
static_assert(std::is_convertible_v<safe::SharedCopyOnWritePtr<Derived>,
                                    safe::SharedCopyOnWritePtr<Base>>);
static_assert(std::is_convertible_v<const safe::SharedCopyOnWritePtr<ConcreteDerived> &,
                                    safe::SharedCopyOnWritePtr<AbstractBase>>);
static_assert(!std::is_convertible_v<safe::SharedCopyOnWritePtr<Base>,
                                     safe::SharedCopyOnWritePtr<Derived>>);

// The way out, to an independent CopyOnWritePtr, has to be spelled out: a
// function declared to return a CopyOnWritePtr would otherwise compile and hand
// out the copies whose writes get lost
static_assert(std::is_constructible_v<safe::CopyOnWritePtr<Tracked>,
                                      const safe::SharedCopyOnWritePtr<Tracked> &>);
static_assert(
    !std::is_convertible_v<safe::SharedCopyOnWritePtr<Tracked>, safe::CopyOnWritePtr<Tracked>>);
static_assert(!std::is_assignable_v<safe::CopyOnWritePtr<Tracked> &,
                                    const safe::SharedCopyOnWritePtr<Tracked> &>);
static_assert(std::is_constructible_v<safe::CopyOnWritePtr<AbstractBase>,
                                      safe::SharedCopyOnWritePtr<ConcreteDerived>>);
static_assert(!std::is_constructible_v<safe::CopyOnWritePtr<Derived>,
                                       safe::SharedCopyOnWritePtr<Base>>);

// An observer is taken like from a SharedPtr
static_assert(
    std::is_convertible_v<const safe::SharedCopyOnWritePtr<Derived> &, safe::WeakPtr<Base>>);
static_assert(
    !std::is_convertible_v<const safe::SharedCopyOnWritePtr<Base> &, safe::WeakPtr<Derived>>);
static_assert(std::is_assignable_v<safe::WeakPtr<Base> &,
                                   const safe::SharedCopyOnWritePtr<Derived> &>);
static_assert(std::is_convertible_v<const safe::CopyOnWritePtr<Derived> &, safe::WeakPtr<Base>>);

// A cast returns the kind of pointer it was given...
static_assert(std::is_same_v<decltype(safe::staticCast<Derived>(
                                 std::declval<const safe::SharedCopyOnWritePtr<Base> &>())),
                             safe::SharedCopyOnWritePtr<Derived>>);
static_assert(std::is_same_v<decltype(safe::dynamicCast<Derived>(
                                 std::declval<const safe::SharedCopyOnWritePtr<Base> &>())),
                             safe::SharedCopyOnWritePtr<Derived>>);
static_assert(std::is_same_v<decltype(safe::constCast<Tracked>(
                                 std::declval<const safe::SharedCopyOnWritePtr<const Tracked> &>())),
                             safe::SharedCopyOnWritePtr<Tracked>>);
static_assert(std::is_same_v<decltype(safe::staticCast<Derived>(
                                 std::declval<const safe::SharedPtr<Base> &>())),
                             safe::SharedPtr<Derived>>);

// ... and a CopyOnWritePtr is not cast at all: whether the result writes in place
// or clones would be anybody's guess. Converting it first makes that a choice.
static_assert(CanStaticCast<safe::SharedPtr<Base>, Derived>);
static_assert(CanStaticCast<safe::SharedCopyOnWritePtr<Base>, Derived>);
static_assert(!CanStaticCast<safe::CopyOnWritePtr<Base>, Derived>);
static_assert(CanDynamicCast<safe::SharedPtr<Base>, Derived>);
static_assert(CanDynamicCast<safe::SharedCopyOnWritePtr<Base>, Derived>);
static_assert(!CanDynamicCast<safe::CopyOnWritePtr<Base>, Derived>);
static_assert(CanConstCast<safe::SharedPtr<const Tracked>, Tracked>);
static_assert(CanConstCast<safe::SharedCopyOnWritePtr<const Tracked>, Tracked>);
static_assert(!CanConstCast<safe::CopyOnWritePtr<const Tracked>, Tracked>);

static_assert(ComparableBothWays<safe::SharedCopyOnWritePtr<Derived>, safe::SharedPtr<Base>>);
static_assert(ComparableBothWays<safe::SharedCopyOnWritePtr<Base>, safe::CopyOnWritePtr<Derived>>);
static_assert(
    ComparableBothWays<safe::SharedCopyOnWritePtr<Derived>, safe::SharedCopyOnWritePtr<Base>>);
static_assert(ComparableBothWays<safe::CopyOnWritePtr<Derived>, safe::CopyOnWritePtr<Base>>);
static_assert(ComparableBothWays<safe::CopyOnWritePtr<Derived>, safe::SharedPtr<Base>>);

// Not a SharedPtr: the block it points to changes behind its back
static_assert(
    !std::is_convertible_v<safe::SharedCopyOnWritePtr<Tracked>, safe::SharedPtr<Tracked>>);
static_assert(!std::is_base_of_v<safe::SharedPtr<Tracked>, safe::SharedCopyOnWritePtr<Tracked>>);
static_assert(!CanLockAll<safe::SharedCopyOnWritePtr<Tracked> &>);

// Writing changes the object and the slot, never the pointer
static_assert(CanWriteLock<safe::SharedCopyOnWritePtr<Tracked> &>);
static_assert(CanWriteLock<const safe::SharedCopyOnWritePtr<Tracked> &>);
static_assert(CanWriteLock<safe::SharedCopyOnWritePtr<Tracked>>);

// A CopyOnWritePtr still has no write lock when it is const
static_assert(CanWriteLock<safe::CopyOnWritePtr<Tracked> &>);
static_assert(!CanWriteLock<const safe::CopyOnWritePtr<Tracked> &>);

// The guards are the ones every other pointer hands out
static_assert(
    std::is_same_v<decltype(std::declval<safe::SharedCopyOnWritePtr<Tracked>>().writeLock()),
                   safe::UniqueLockGuard<Tracked, std::shared_mutex>>);
static_assert(
    std::is_same_v<decltype(std::declval<safe::SharedCopyOnWritePtr<Tracked>>().readLock()),
                   safe::SharedLockGuard<Tracked, std::shared_mutex>>);
static_assert(std::is_same_v<safe::SharedCopyOnWritePtr<Tracked>::ElementType, Tracked>);
static_assert(std::is_same_v<safe::SharedCopyOnWritePtr<Tracked, InstrumentedMutex>::MutexType,
                             InstrumentedMutex>);

static_assert(std::is_nothrow_copy_constructible_v<safe::SharedCopyOnWritePtr<Tracked>>);
static_assert(std::is_nothrow_move_constructible_v<safe::SharedCopyOnWritePtr<Tracked>>);
static_assert(std::is_nothrow_copy_assignable_v<safe::SharedCopyOnWritePtr<Tracked>>);
static_assert(std::is_nothrow_move_assignable_v<safe::SharedCopyOnWritePtr<Tracked>>);

// A container moves its elements when it grows, it must not have to copy them
static_assert(std::is_nothrow_move_constructible_v<safe::CopyOnWritePtr<Tracked>>);
static_assert(std::is_nothrow_copy_constructible_v<safe::CopyOnWritePtr<Tracked>>);

struct ConstexprBase
{
    virtual constexpr ~ConstexprBase() = default;
};

struct ConstexprDerived : ConstexprBase
{};

constexpr bool emptySharedCopyOnWriteApi()
{
    safe::SharedCopyOnWritePtr<int> first;
    safe::SharedCopyOnWritePtr<int> second{nullptr};
    safe::SharedCopyOnWritePtr<int> copy(first);
    safe::SharedCopyOnWritePtr<int> moved(std::move(copy));

    second = first;
    moved = std::move(second);
    first = nullptr;
    first.reset();
    swap(first, moved);

    safe::SharedCopyOnWritePtr<const int> readOnly = first;
    safe::SharedCopyOnWritePtr<const int> movedReadOnly = std::move(first);
    readOnly = moved;
    readOnly = std::move(moved);

    // Everything that leaves an empty pointer is empty too
    safe::SharedCopyOnWritePtr<ConstexprBase> base;
    safe::WeakPtr<int> observer = first;
    observer = first;
    safe::CopyOnWritePtr<int> branch(first);
    safe::CopyOnWritePtr<const int> readOnlyBranch(first);
    auto staticallyCast = safe::staticCast<ConstexprDerived>(base);
    auto dynamicallyCast = safe::dynamicCast<ConstexprDerived>(base);
    auto writable = safe::constCast<int>(readOnly);

    return !first && !second && !copy && !moved && !readOnly && !movedReadOnly
           && !first.snapshot() && first == nullptr && first.owner() == nullptr
           && first.useCount() == 0 && first.weakCount() == 0
           && (first <=> moved) == std::strong_ordering::equal && observer.expired()
           && !branch && !readOnlyBranch && !staticallyCast && !dynamicallyCast && !writable
           && first == safe::SharedPtr<int>{} && safe::SharedPtr<int>{} == first
           && first == branch;
}

// An empty CopyOnWritePtr has no slot, and none of what a slot made it need
constexpr bool emptyCopyOnWriteApi()
{
    safe::CopyOnWritePtr<int> first;
    safe::CopyOnWritePtr<int> copy(first);
    safe::CopyOnWritePtr<int> moved(std::move(copy));
    first = moved;
    first = std::move(moved);
    first.reset();
    first.swap(moved);
    swap(first, moved);

    safe::SharedPtr<int> plain = first;
    safe::SharedPtr<const int> movedPlain = std::move(copy);
    plain = first;
    safe::WeakPtr<int> observer = first;
    observer = first;

    return !first && first == nullptr && first == moved && first == plain && plain == first
           && first.owner() == nullptr && first.useCount() == 0 && first.weakCount() == 0
           && (first <=> moved) == std::strong_ordering::equal && !plain && !movedPlain
           && observer.expired() && safe::getDeleter<int>(first) == nullptr;
}

static_assert(emptySharedCopyOnWriteApi());
static_assert(emptyCopyOnWriteApi());

// A slot of its own that shares the object of source, the way a copy of source
// would. Made from source itself the pointer would share the slot of source.
template<typename T>
safe::SharedCopyOnWritePtr<T> branchOf(const safe::CopyOnWritePtr<T> &source)
{
    return safe::SharedCopyOnWritePtr<T>(safe::CopyOnWritePtr<T>(source));
}

// Knows nothing about copy on write: takes the usual SharedPtr view and keeps it
void writeThroughAView(const safe::SharedPtr<Tracked> &pointer, int value)
{
    safe::SharedPtr<Tracked> kept = pointer;
    kept.writeLock()->value = value;
}

} // namespace

TEST(SharedCopyOnWrite, isEmptyByDefault)
{
    safe::SharedCopyOnWritePtr<Tracked> empty;
    safe::SharedCopyOnWritePtr<Tracked> fromNull = nullptr;

    CHECK(!empty);
    CHECK(empty == nullptr);
    CHECK(empty == fromNull);
    CHECK(empty.owner() == nullptr);
    CHECK_EQ(empty.useCount(), std::size_t(0));
    CHECK(!empty.snapshot());

    // An empty CopyOnWritePtr has nothing to share, and stays as it is
    safe::CopyOnWritePtr<Tracked> none;
    safe::SharedCopyOnWritePtr<Tracked> fromEmpty = none;
    safe::SharedCopyOnWritePtr<Tracked> fromEmptyTemporary{safe::CopyOnWritePtr<Tracked>{}};
    CHECK(!fromEmpty);
    CHECK(!fromEmptyTemporary);
    CHECK(!none);
}

TEST(SharedCopyOnWrite, copiesShareOneSlot)
{
    auto pointer = safe::makeSharedCopyOnWrite<Tracked>(1);
    CHECK(bool(pointer));
    CHECK_EQ(pointer.useCount(), std::size_t(1));

    auto copy = pointer;
    CHECK(copy == pointer);
    CHECK(copy.owner() == pointer.owner());
    CHECK_EQ(pointer.useCount(), std::size_t(2));

    auto moved = std::move(copy);
    CHECK(!copy);
    CHECK(moved == pointer);
    CHECK_EQ(pointer.useCount(), std::size_t(2));

    {
        auto guard = pointer.readLock();
        CHECK_EQ(pointer.useCount(), std::size_t(2)); // pointers only, not their guards
    }
    CHECK_EQ(pointer.useCount(), std::size_t(2));
}

// ---------------------------------------------------------------------------
// The use case: a container of CopyOnWritePtr and functions returning its
// elements by value

TEST(SharedCopyOnWrite, handsOutElementsOfAContainerByValue)
{
    std::vector<safe::CopyOnWritePtr<Value>> first;
    first.push_back(safe::makeCopyOnWrite<DerivedValue>());
    first.push_back(safe::makeCopyOnWrite<DerivedValue>());
    std::vector<safe::CopyOnWritePtr<Value>> second = first;

    const auto getVal = [&second](std::size_t index) -> safe::SharedCopyOnWritePtr<Value> {
        return second[index];
    };

    auto v1 = getVal(0);
    auto v2 = getVal(0);
    getVal(0).writeLock()->val = 1; // through a temporary
    CHECK_EQ(v1.readLock()->val, 1);
    CHECK_EQ(v2.readLock()->val, 1);
    CHECK_EQ(second[0].readLock()->val, 1);
    CHECK_EQ(first[0].readLock()->val, 10);

    // Down to the complete type. A cast returns what it is given: through the
    // SharedCopyOnWritePtr it stays on the slot of second[0]...
    safe::staticCast<DerivedValue>(getVal(0)).writeLock()->other = 2;
    CHECK_EQ(safe::staticCast<DerivedValue>(getVal(0)).readLock()->other, 2);
    CHECK_EQ(safe::staticCast<DerivedValue>(v1).readLock()->other, 2);
    // ... and through a SharedPtr it is a plain look at the object
    CHECK_EQ(safe::staticCast<DerivedValue>(safe::SharedPtr<Value>(second[0])).readLock()->other,
             2);
    CHECK_EQ(safe::staticCast<DerivedValue>(safe::SharedPtr<Value>(first[0])).readLock()->other,
             20);

    // The same for an element nothing was written to before: the write through
    // the cast is the one that takes second[1] away from first[1]
    safe::staticCast<DerivedValue>(getVal(1)).writeLock()->other = 3;
    CHECK_EQ(safe::staticCast<DerivedValue>(getVal(1)).readLock()->other, 3);
    CHECK_EQ(second[1].readLock()->val, 10);
    CHECK_EQ(safe::staticCast<DerivedValue>(safe::SharedPtr<Value>(first[1])).readLock()->other,
             20);
}

TEST(SharedCopyOnWrite, handsOutElementsAsABaseClass)
{
    std::vector<safe::CopyOnWritePtr<DerivedValue>> first;
    first.push_back(safe::makeCopyOnWrite<DerivedValue>());
    std::vector<safe::CopyOnWritePtr<DerivedValue>> second = first;

    const auto getVal = [&second](std::size_t index) -> safe::SharedCopyOnWritePtr<Value> {
        return second[index];
    };

    auto v1 = getVal(0);
    auto v2 = getVal(0);
    getVal(0).writeLock()->val = 1;
    CHECK_EQ(v1.readLock()->val, 1);
    CHECK_EQ(v2.readLock()->val, 1);
    CHECK_EQ(second[0].readLock()->val, 1);
    CHECK_EQ(second[0].readLock()->other, 20); // the clone is a whole DerivedValue
    CHECK_EQ(first[0].readLock()->val, 10);
}

TEST(SharedCopyOnWrite, everyCloneIsTheCompleteObjectWhicheverViewWrites)
{
    // Created as a Derived, kept, handed out and written to as a Base
    std::vector<safe::CopyOnWritePtr<Base>> first;
    first.push_back(safe::makeCopyOnWrite<Derived>());
    auto second = first;
    const auto isDerived = [](const safe::CopyOnWritePtr<Base> &pointer) {
        auto derived = safe::dynamicCast<Derived>(safe::SharedPtr<Base>(pointer));
        return derived && derived.readLock()->derived == 2;
    };

    safe::SharedCopyOnWritePtr<Base> handle = second[0];
    handle.writeLock()->base = 7; // cloned through the SharedCopyOnWritePtr<Base>
    CHECK(second[0] != first[0]);
    CHECK(isDerived(second[0]));
    CHECK(bool(safe::dynamicCast<Derived>(handle)));

    auto third = second;
    second[0].writeLock()->base = 8; // through the CopyOnWritePtr<Base> itself
    CHECK(second[0] != third[0]);
    CHECK(isDerived(second[0]));

    auto fourth = second;
    handle.writeLock()->base = 9; // through the slot again
    CHECK(isDerived(second[0]));

    auto snapshot = handle.snapshot();
    snapshot.writeLock()->base = 10; // and through a snapshot
    CHECK(isDerived(snapshot));

    CHECK(isDerived(first[0]));
    CHECK(isDerived(third[0]));
    CHECK(isDerived(fourth[0]));
    CHECK_EQ(first[0].readLock()->base, 1);
    CHECK_EQ(third[0].readLock()->base, 7);
    CHECK_EQ(fourth[0].readLock()->base, 8);
    CHECK_EQ(second[0].readLock()->base, 9);
    CHECK_EQ(snapshot.readLock()->base, 10);
}

TEST(SharedCopyOnWrite, aWriteThroughAnyCopyReachesAllOfThem)
{
    Tracked::resetCounters();
    {
        std::vector<safe::CopyOnWritePtr<Tracked>> original;
        for (int i = 0; i < 3; ++i)
            original.push_back(safe::makeCopyOnWrite<Tracked>(i));

        auto working = original;
        const auto item = [&working](std::size_t index) -> safe::SharedCopyOnWritePtr<Tracked> {
            return working[index];
        };
        CHECK_EQ(Tracked::copied.load(), 0); // nothing is cloned up front

        auto early = item(1);
        item(1).writeLock()->value = 10; // through a temporary
        CHECK_EQ(Tracked::copied.load(), 1);

        CHECK_EQ(working[1].readLock()->value, 10);
        CHECK_EQ(early.readLock()->value, 10);
        CHECK_EQ(item(1).readLock()->value, 10);
        CHECK_EQ(original[1].readLock()->value, 1); // the other container keeps its value

        // Later writes find the object unshared and happen in place, whoever
        // makes them
        early.writeLock()->value = 11;
        item(1).writeLock()->value += 1;
        working[1].writeLock()->value += 1;
        CHECK_EQ(Tracked::copied.load(), 1);
        CHECK_EQ(early.readLock()->value, 13);
        CHECK_EQ(original[1].readLock()->value, 1);

        // The neighbours were never written to and still share with the original
        CHECK(working[0] == original[0]);
        CHECK(working[2] == original[2]);
        CHECK(working[1] != original[1]);
        CHECK(item(1) == working[1]);
        CHECK_EQ(Tracked::alive.load(), 4);
    }
    CHECK_EQ(Tracked::alive.load(), 0);
}

TEST(SharedCopyOnWrite, everyPointerMadeFromOneCopyOnWritePointerSharesItsSlot)
{
    auto element = safe::makeCopyOnWrite<Tracked>(1);
    safe::SharedCopyOnWritePtr<Tracked> first = element;
    safe::SharedCopyOnWritePtr<Tracked> second = element;

    CHECK(first == second);
    CHECK(first.owner() == second.owner());
    CHECK_EQ(first.useCount(), std::size_t(3)); // the two and element

    second.writeLock()->value = 2;
    CHECK_EQ(first.readLock()->value, 2);
    CHECK_EQ(element.readLock()->value, 2);

    // A copy of the CopyOnWritePtr is another pointer, with a slot of its own
    auto copy = element;
    safe::SharedCopyOnWritePtr<Tracked> third = copy;
    CHECK(third != first);
    CHECK_EQ(first.useCount(), std::size_t(3));

    third.writeLock()->value = 3;
    CHECK_EQ(copy.readLock()->value, 3);
    CHECK_EQ(first.readLock()->value, 2);
    CHECK_EQ(element.readLock()->value, 2);
}

TEST(SharedCopyOnWrite, thePointerItWasMadeFromFollowsEveryWrite)
{
    Tracked::resetCounters();
    auto original = safe::makeCopyOnWrite<Tracked>(1);
    auto element = original;
    safe::SharedCopyOnWritePtr<Tracked> handle = element;
    CHECK(element == handle);
    CHECK(element == original);

    handle.writeLock()->value = 2; // clones, and takes element along
    CHECK_EQ(Tracked::copied.load(), 1);
    CHECK_EQ(element.readLock()->value, 2);
    CHECK_EQ(original.readLock()->value, 1);
    CHECK(element == handle);
    CHECK(handle == element);
    CHECK(element != original);

    element.writeLock()->value = 3; // and the other way round
    CHECK_EQ(handle.readLock()->value, 3);
    CHECK_EQ(Tracked::copied.load(), 1);

    // Everything else a pointer can be asked comes from the slot as well
    CHECK(bool(element));
    CHECK(element != nullptr);
    CHECK(element.owner() == handle.snapshot().owner());
    CHECK_EQ(element.useCount(), std::size_t(1)); // the slot is the one owner of the object
    CHECK_EQ(element.weakCount(), std::size_t(1));
}

TEST(SharedCopyOnWrite, aCopyOfTheCopyOnWritePointerIsStillIndependent)
{
    auto element = safe::makeCopyOnWrite<Tracked>(1);
    safe::SharedCopyOnWritePtr<Tracked> handle = element;

    auto branch = element; // shares the object, not the slot
    CHECK(branch == element);

    handle.writeLock()->value = 2;
    CHECK_EQ(element.readLock()->value, 2);
    CHECK_EQ(branch.readLock()->value, 1);
    CHECK(branch != element);

    branch.writeLock()->value = 5; // clones for itself, like any CopyOnWritePtr
    CHECK_EQ(branch.readLock()->value, 5);
    CHECK_EQ(element.readLock()->value, 2);
    CHECK_EQ(handle.readLock()->value, 2);

    // A copy made after the write is a copy of what the slot has by then
    auto later = element;
    CHECK_EQ(later.readLock()->value, 2);
    handle.writeLock()->value = 3;
    CHECK_EQ(later.readLock()->value, 2);
}

TEST(SharedCopyOnWrite, copyingTheContainerStartsANewBranch)
{
    Tracked::resetCounters();
    std::vector<safe::CopyOnWritePtr<Tracked>> second;
    second.push_back(safe::makeCopyOnWrite<Tracked>(10));
    second.push_back(safe::makeCopyOnWrite<Tracked>(20));
    safe::SharedCopyOnWritePtr<Tracked> handle = second[1];
    second[1].writeLock()->value = 21; // in place, nobody else has it

    auto third = second;
    CHECK_EQ(Tracked::copied.load(), 0);
    CHECK_EQ(third[1].readLock()->value, 21);

    handle.writeLock()->value = 300; // second[1] leaves third[1] behind
    CHECK_EQ(Tracked::copied.load(), 1);
    CHECK_EQ(second[1].readLock()->value, 300);
    CHECK_EQ(third[1].readLock()->value, 21);

    third[0].writeLock()->value = 11;
    CHECK_EQ(Tracked::copied.load(), 2);
    CHECK_EQ(second[0].readLock()->value, 10);
}

TEST(SharedCopyOnWrite, pointersOutliveTheContainerTheyCameFrom)
{
    Tracked::resetCounters();
    {
        auto first = safe::makeCopyOnWrite<Tracked>(1);
        safe::SharedCopyOnWritePtr<Tracked> escapedA;
        safe::SharedCopyOnWritePtr<Tracked> escapedB;
        {
            std::vector<safe::CopyOnWritePtr<Tracked>> elements;
            elements.push_back(first);
            escapedA = elements[0];
            escapedB = elements[0];

            // Growing moves elements[0] to another address, the slot goes along
            for (int i = 0; i < 64; ++i)
                elements.push_back(safe::makeCopyOnWrite<Tracked>(i + 10));

            escapedA.writeLock()->value = 55;
            CHECK_EQ(first.readLock()->value, 1);
            CHECK_EQ(elements[0].readLock()->value, 55);
            CHECK_EQ(escapedB.readLock()->value, 55);
            CHECK(elements[0] == escapedA);
        }

        // The container is gone, the two pointers still share the slot
        CHECK_EQ(escapedA.useCount(), std::size_t(2));
        CHECK_EQ(escapedA.readLock()->value, 55);
        escapedB.writeLock()->value = 66;
        CHECK_EQ(escapedA.readLock()->value, 66);
        CHECK_EQ(Tracked::copied.load(), 1);
        CHECK_EQ(Tracked::alive.load(), 2);
    }
    CHECK_EQ(Tracked::alive.load(), 0);
}

// ---------------------------------------------------------------------------
// What making a SharedCopyOnWritePtr leaves behind in the CopyOnWritePtr

TEST(SharedCopyOnWrite, theFirstWriteClonesWhileThePointerStillHoldsItsOwnReference)
{
    // Making a SharedCopyOnWritePtr only reads the CopyOnWritePtr, so that any
    // number of threads can do it at once. The reference the pointer holds
    // itself stays where it is, and until it is gone the object looks shared.
    Tracked::resetCounters();
    {
        auto element = safe::makeCopyOnWrite<Tracked>(1);
        safe::SharedCopyOnWritePtr<Tracked> handle = element;

        handle.writeLock()->value = 2;
        CHECK_EQ(Tracked::copied.load(), 1); // although nobody else had the object
        CHECK_EQ(element.readLock()->value, 2);
        CHECK_EQ(Tracked::alive.load(), 2); // and the pointer keeps the previous one alive

        handle.writeLock()->value = 3; // only that once
        CHECK_EQ(Tracked::copied.load(), 1);

        // Until it is used in a way that needs it for itself anyway
        element.writeLock()->value = 4;
        CHECK_EQ(Tracked::alive.load(), 1);
        CHECK_EQ(Tracked::copied.load(), 1);
        CHECK_EQ(handle.readLock()->value, 4);
    }
    CHECK_EQ(Tracked::alive.load(), 0);
}

TEST(SharedCopyOnWrite, aWriteLockOnThePointerItselfGivesUpItsOwnReference)
{
    Tracked::resetCounters();
    auto element = safe::makeCopyOnWrite<Tracked>(1);
    safe::SharedCopyOnWritePtr<Tracked> handle = element;

    element.writeLock()->value = 2;
    handle.writeLock()->value = 3; // the object is the slot's alone now
    CHECK_EQ(Tracked::copied.load(), 0);
    CHECK_EQ(Tracked::alive.load(), 1);
    CHECK_EQ(element.readLock()->value, 3);
}

TEST(SharedCopyOnWrite, movingThePointerGivesUpItsOwnReference)
{
    Tracked::resetCounters();
    std::vector<safe::CopyOnWritePtr<Tracked>> elements;
    elements.push_back(safe::makeCopyOnWrite<Tracked>(1));
    safe::SharedCopyOnWritePtr<Tracked> handle = elements[0];

    for (int i = 0; i < 64; ++i) // growing moves elements[0]
        elements.push_back(safe::makeCopyOnWrite<Tracked>(i + 10));

    handle.writeLock()->value = 2;
    CHECK_EQ(Tracked::copied.load(), 0);
    CHECK_EQ(elements[0].readLock()->value, 2);
}

TEST(SharedCopyOnWrite, theObjectLeftBehindGoesOnceThePointerIsModified)
{
    Tracked::resetCounters();
    auto original = safe::makeCopyOnWrite<Tracked>(1);
    auto element = original;
    safe::SharedCopyOnWritePtr<Tracked> handle = element;

    handle.writeLock()->value = 2;
    original.reset();
    CHECK_EQ(Tracked::alive.load(), 2); // element still holds the object it started with
    CHECK_EQ(element.readLock()->value, 2); // without looking at it any more

    auto moved = std::move(element);
    CHECK_EQ(Tracked::alive.load(), 1);
    CHECK_EQ(moved.readLock()->value, 2);
    CHECK(moved == handle);
}

TEST(SharedCopyOnWrite, aWriteAfterTheOtherOwnerWroteFirst)
{
    Tracked::resetCounters();
    auto original = safe::makeCopyOnWrite<Tracked>(1);
    auto element = original;
    safe::SharedCopyOnWritePtr<Tracked> handle = element;

    original.writeLock()->value = 2; // the other owner clones for itself
    CHECK_EQ(Tracked::copied.load(), 1);
    CHECK_EQ(handle.readLock()->value, 1);

    // See above: element still holds its own reference, so this clones as well
    handle.writeLock()->value = 3;
    CHECK_EQ(Tracked::copied.load(), 2);
    CHECK_EQ(original.readLock()->value, 2);
    CHECK_EQ(element.readLock()->value, 3);
}

TEST(SharedCopyOnWrite, assigningToTheCopyOnWritePointerLeavesItsSharedPointersBehind)
{
    auto element = safe::makeCopyOnWrite<Tracked>(1);
    auto other = safe::makeCopyOnWrite<Tracked>(2);
    safe::SharedCopyOnWritePtr<Tracked> handle = element;

    element = other; // a different pointer from here on
    CHECK_EQ(element.readLock()->value, 2);
    CHECK_EQ(handle.readLock()->value, 1);
    CHECK(handle != element);
    CHECK(element == other);
    CHECK_EQ(handle.useCount(), std::size_t(1));

    // Which starts over with a slot of its own
    safe::SharedCopyOnWritePtr<Tracked> fresh = element;
    CHECK(fresh != handle);
    CHECK(fresh == element);
    fresh.writeLock()->value = 3;
    CHECK_EQ(element.readLock()->value, 3);
    CHECK_EQ(other.readLock()->value, 2);

    element.reset();
    CHECK(!element);
    CHECK(element == nullptr);
    CHECK_EQ(fresh.readLock()->value, 3);
    CHECK_EQ(fresh.useCount(), std::size_t(1));

    element = nullptr;
    CHECK(!element);
}

TEST(SharedCopyOnWrite, movingTheCopyOnWritePointerTakesItsSharedPointersAlong)
{
    auto original = safe::makeCopyOnWrite<Both>();
    auto element = original;
    safe::SharedCopyOnWritePtr<Both> handle = element;

    safe::CopyOnWritePtr<Both> moved = std::move(element);
    CHECK(!element);
    CHECK(moved == handle);
    handle.writeLock()->both = 31;
    CHECK_EQ(moved.readLock()->both, 31);

    // To a base class that does not start where the object starts
    safe::CopyOnWritePtr<Right> asRight = std::move(moved);
    CHECK(!moved);
    CHECK(asRight == handle);
    CHECK_EQ(asRight.readLock()->right, 20);

    asRight.writeLock()->right = 21;
    CHECK_EQ(handle.readLock()->right, 21);
    CHECK_EQ(handle.readLock()->both, 31);

    handle.writeLock()->right = 22;
    CHECK_EQ(asRight.readLock()->right, 22);

    safe::SharedCopyOnWritePtr<Right> again = asRight;
    CHECK(again == handle);
    CHECK_EQ(again.readLock()->right, 22);

    // A copy to a base class is as independent as any other copy
    auto third = safe::makeCopyOnWrite<Both>();
    safe::SharedCopyOnWritePtr<Both> thirdHandle = third;
    thirdHandle.writeLock()->right = 40;
    safe::CopyOnWritePtr<Right> branch = third;
    thirdHandle.writeLock()->right = 41;
    CHECK_EQ(branch.readLock()->right, 40);
    CHECK_EQ(third.readLock()->right, 41);

    CHECK_EQ(original.readLock()->right, 20);
    CHECK_EQ(original.readLock()->both, 30);
}

TEST(SharedCopyOnWrite, swappingTwoCopyOnWritePointersSwapsTheirSlots)
{
    auto a = safe::makeCopyOnWrite<Tracked>(1);
    auto b = safe::makeCopyOnWrite<Tracked>(2);
    safe::SharedCopyOnWritePtr<Tracked> forA = a;
    safe::SharedCopyOnWritePtr<Tracked> forB = b;

    swap(a, b);
    CHECK(a == forB);
    CHECK(b == forA);
    CHECK_EQ(a.readLock()->value, 2);

    forB.writeLock()->value = 3;
    CHECK_EQ(a.readLock()->value, 3);
    CHECK_EQ(b.readLock()->value, 1);

    // With a pointer that shares nothing
    auto plain = safe::makeCopyOnWrite<Tracked>(9);
    a.swap(plain);
    CHECK(plain == forB);
    CHECK_EQ(a.readLock()->value, 9);
    forB.writeLock()->value = 4;
    CHECK_EQ(plain.readLock()->value, 4);
    CHECK_EQ(a.readLock()->value, 9);
}

TEST(SharedCopyOnWrite, aSharedPointerViewOfTheCopyOnWritePointerHasWhatTheSlotHas)
{
    Tracked::resetCounters();
    auto original = safe::makeCopyOnWrite<Tracked>(1);
    auto element = original;
    safe::SharedCopyOnWritePtr<Tracked> handle = element;
    handle.writeLock()->value = 2; // moves element to a clone, original keeps 1

    // The view is made from the slot: what gets written, in place, is the object
    // element has now, not the one it had before
    writeThroughAView(element, 3);
    CHECK_EQ(handle.readLock()->value, 3);
    CHECK_EQ(element.readLock()->value, 3);
    CHECK_EQ(original.readLock()->value, 1);
    CHECK_EQ(Tracked::copied.load(), 1);

    const safe::SharedPtr<Tracked> &view = element;
    CHECK(bool(view));
    CHECK(view == element);
    CHECK(view == handle);
    CHECK(view != original);

    // A write through the view goes to the object for every owner, it never clones
    auto branch = element;
    view.writeLock()->value = 4;
    CHECK_EQ(branch.readLock()->value, 4);
    CHECK_EQ(handle.readLock()->value, 4);
    CHECK_EQ(Tracked::copied.load(), 1);

    handle.writeLock()->value = 5; // an ordinary write still clones
    CHECK_EQ(Tracked::copied.load(), 2);
    CHECK_EQ(branch.readLock()->value, 4);

    // Still the slot's object once element gave up its own reference, by a write
    // of its own or by a move
    element.writeLock()->value = 6;
    auto moved = std::move(element);
    writeThroughAView(moved, 7);
    CHECK_EQ(handle.readLock()->value, 7);
    CHECK_EQ(Tracked::copied.load(), 2);

    const safe::SharedPtr<Tracked> &movedView = moved;
    auto other = safe::makeShared<Tracked>(0);
    auto locked = safe::lockAll(movedView, other);
    CHECK_EQ(locked.get<0>().value, 7);
}

TEST(SharedCopyOnWrite, convertingTheCopyOnWritePointerGivesWhatTheSlotHas)
{
    auto original = safe::makeCopyOnWrite<Tracked>(1);
    auto element = original;
    safe::SharedCopyOnWritePtr<Tracked> handle = element;
    handle.writeLock()->value = 2;

    safe::SharedPtr<Tracked> plain = element;
    CHECK_EQ(plain.readLock()->value, 2);
    CHECK(handle == plain);
    CHECK(element == plain);
    CHECK(plain == element);

    safe::SharedPtr<Tracked> assigned;
    assigned = element;
    CHECK(assigned == plain);

    safe::WeakPtr<Tracked> observer = element;
    CHECK_EQ(observer.lock().readLock()->value, 2);
    observer = original;
    CHECK_EQ(observer.lock().readLock()->value, 1);
    observer = element;
    CHECK_EQ(observer.lock().readLock()->value, 2);

    CHECK(safe::getDeleter<int>(element) == nullptr);

    // An alias of something inside the object. The plain pointers above are
    // other owners, so this write lock moves the slot on once more.
    int *value = nullptr;
    {
        auto guard = element.writeLock();
        value = &guard->value;
    }
    safe::SharedPtr<int> alias(element, value);
    CHECK_EQ(*alias.readLock(), 2);
    CHECK(alias == handle);
    CHECK(alias != plain);

    safe::SharedPtr<Tracked> moved = std::move(element);
    CHECK(!element);
    CHECK(handle == moved);
    CHECK_EQ(handle.useCount(), std::size_t(1));
}

// ---------------------------------------------------------------------------
// Pointers that have a slot to themselves

TEST(SharedCopyOnWrite, readingNeverClones)
{
    Tracked::resetCounters();
    auto source = safe::makeCopyOnWrite<Tracked>(1);
    auto element = source;
    safe::SharedCopyOnWritePtr<Tracked> pointer = element;
    auto copy = pointer;

    CHECK_EQ(pointer.readLock()->value, 1);
    CHECK_EQ(copy.readLock()->value, 1);
    CHECK_EQ(element.readLock()->value, 1);
    CHECK_EQ(Tracked::copied.load(), 0);
    CHECK(pointer == source);
}

TEST(SharedCopyOnWrite, writesInPlaceWhileNothingElseOwnsTheObject)
{
    Tracked::resetCounters();
    auto pointer = safe::makeSharedCopyOnWrite<Tracked>(1);
    auto copy = pointer;

    pointer.writeLock()->value = 2;
    copy.writeLock()->value += 1;
    {
        auto guard = pointer.readLock(); // a guard that is gone leaves no trace
    }
    pointer.writeLock()->value += 1;

    CHECK_EQ(Tracked::copied.load(), 0);
    CHECK_EQ(Tracked::alive.load(), 1);
    CHECK_EQ(copy.readLock()->value, 4);
}

TEST(SharedCopyOnWrite, takesOverATemporaryCopyOnWritePointer)
{
    Tracked::resetCounters();
    auto source = safe::makeCopyOnWrite<Tracked>(1);
    const void *object = source.owner();

    safe::SharedCopyOnWritePtr<Tracked> pointer(std::move(source));
    CHECK(!source);
    CHECK(pointer.snapshot().owner() == object);

    pointer.writeLock()->value = 2; // the only owner, nothing to clone
    CHECK_EQ(Tracked::copied.load(), 0);
    CHECK_EQ(pointer.readLock()->value, 2);

    // A temporary that shares a slot already: the result joins that slot
    auto element = safe::makeCopyOnWrite<Tracked>(5);
    safe::SharedCopyOnWritePtr<Tracked> first = element;
    safe::SharedCopyOnWritePtr<Tracked> second(std::move(element));
    CHECK(!element);
    CHECK(second == first);
    CHECK_EQ(first.useCount(), std::size_t(2));
}

TEST(SharedCopyOnWrite, aSlotOfItsOwnIsIndependentOfThePointerItCopied)
{
    Tracked::resetCounters();
    auto source = safe::makeCopyOnWrite<Tracked>(1);
    auto first = branchOf(source);
    auto second = branchOf(source);

    CHECK(first != second);
    CHECK(first == source); // still the one object
    first.writeLock()->value = 2;
    second.writeLock()->value = 3;

    CHECK_EQ(source.readLock()->value, 1);
    CHECK_EQ(first.readLock()->value, 2);
    CHECK_EQ(second.readLock()->value, 3);
    CHECK_EQ(Tracked::copied.load(), 2);

    // The owner that is left writes in place
    source.writeLock()->value = 4;
    CHECK_EQ(Tracked::copied.load(), 2);
}

TEST(SharedCopyOnWrite, snapshotKeepsTheValueItWasTakenAt)
{
    Tracked::resetCounters();
    auto pointer = safe::makeSharedCopyOnWrite<Tracked>(1);

    auto snapshot = pointer.snapshot();
    CHECK_EQ(Tracked::copied.load(), 0); // taking it clones nothing

    pointer.writeLock()->value = 2; // the slot moves away from the snapshot
    CHECK_EQ(Tracked::copied.load(), 1);
    CHECK_EQ(snapshot.readLock()->value, 1);
    CHECK_EQ(pointer.readLock()->value, 2);

    pointer.writeLock()->value = 3; // and is on its own again
    CHECK_EQ(Tracked::copied.load(), 1);
    CHECK_EQ(snapshot.readLock()->value, 1);
}

TEST(SharedCopyOnWrite, snapshotIsAnOrdinaryCopyOnWritePointer)
{
    Tracked::resetCounters();
    auto pointer = safe::makeSharedCopyOnWrite<Tracked>(1);
    auto copy = pointer;

    auto snapshot = pointer.snapshot();
    snapshot.writeLock()->value = 5; // clones for itself, the slot is not touched
    CHECK_EQ(Tracked::copied.load(), 1);
    CHECK_EQ(pointer.readLock()->value, 1);
    CHECK_EQ(copy.readLock()->value, 1);

    // The slot is the only owner of its object again
    pointer.writeLock()->value = 2;
    CHECK_EQ(Tracked::copied.load(), 1);
    CHECK_EQ(snapshot.readLock()->value, 5);

    // A second set of pointers, independent of the first
    safe::SharedCopyOnWritePtr<Tracked> forked(pointer.snapshot());
    CHECK(forked != pointer);
    forked.writeLock()->value = 9;
    CHECK_EQ(pointer.readLock()->value, 2);
    CHECK_EQ(copy.readLock()->value, 2);
}

TEST(SharedCopyOnWrite, aDroppedSnapshotCostsNothing)
{
    Tracked::resetCounters();
    auto pointer = safe::makeSharedCopyOnWrite<Tracked>(1);
    {
        auto snapshot = pointer.snapshot();
        CHECK_EQ(snapshot.readLock()->value, 1);
    }
    pointer.writeLock()->value = 2;
    CHECK_EQ(Tracked::copied.load(), 0);
}

TEST(SharedCopyOnWrite, snapshotTakenUnderAWriteLockShowsWhatTheWriteLeavesBehind)
{
    // Documented: copying the value in the middle of a write cannot give a
    // consistent "before", so the snapshot is of the finished write
    Tracked::resetCounters();
    auto pointer = safe::makeSharedCopyOnWrite<Tracked>(1);
    safe::CopyOnWritePtr<Tracked> snapshot;
    {
        auto guard = pointer.writeLock();
        guard->value = 2;
        snapshot = pointer.snapshot();
        guard->value = 3;
    }
    CHECK_EQ(snapshot.readLock()->value, 3);

    pointer.writeLock()->value = 4; // from here on the snapshot is left alone
    CHECK_EQ(snapshot.readLock()->value, 3);
    CHECK_EQ(Tracked::copied.load(), 1);
}

TEST(SharedCopyOnWrite, aReaderOfTheSharedObjectKeepsItsValueWhileTheSlotMovesOn)
{
    // The one write that does not wait for the readers of its slot: it does not
    // write to the object they are reading
    Tracked::resetCounters();
    {
        auto source = safe::makeCopyOnWrite<Tracked>(1);
        auto pointer = branchOf(source);

        std::latch reading{1};
        std::latch written{1};
        {
            std::jthread reader([pointer, &reading, &written] {
                auto guard = pointer.readLock();
                reading.count_down();
                written.wait();
                CHECK_EQ(guard->value, 1); // still the object it locked
            });

            reading.wait();
            pointer.writeLock()->value = 2; // would never return if it waited
            CHECK_EQ(pointer.readLock()->value, 2);

            // The guard of the previous object does not make the new one look shared
            pointer.writeLock()->value = 3;
            CHECK_EQ(Tracked::copied.load(), 1);

            source.reset(); // the guard alone keeps the previous object alive
            CHECK_EQ(Tracked::alive.load(), 2);
            written.count_down();
        }
        CHECK_EQ(Tracked::alive.load(), 1);
        CHECK_EQ(pointer.readLock()->value, 3);
    }
    CHECK_EQ(Tracked::alive.load(), 0);
}

TEST(SharedCopyOnWrite, guardOutlivingEveryPointerKeepsTheSlotAlive)
{
    Tracked::resetCounters();
    {
        auto source = safe::makeCopyOnWrite<Tracked>(1);
        auto guard = [&source] {
            auto pointer = branchOf(source);
            return pointer.writeLock(); // clones, then locks the clone
        }();
        CHECK_EQ(Tracked::alive.load(), 2); // the source plus the clone

        guard->value = 2;
        CHECK_EQ(guard->value, 2);
        CHECK_EQ(source.readLock()->value, 1);
    }
    CHECK_EQ(Tracked::alive.load(), 0);

    // The same when the slot belonged to a CopyOnWritePtr
    {
        auto guard = [] {
            auto element = safe::makeCopyOnWrite<Tracked>(1);
            safe::SharedCopyOnWritePtr<Tracked> pointer = element;
            return element.writeLock();
        }();
        CHECK_EQ(Tracked::alive.load(), 1);
        guard->value = 3;
        CHECK_EQ(guard->value, 3);
    }
    CHECK_EQ(Tracked::alive.load(), 0);
}

TEST(SharedCopyOnWrite, writesThroughAConstPointer)
{
    const auto pointer = safe::makeSharedCopyOnWrite<Tracked>(1);
    pointer.writeLock()->value = 2;
    CHECK_EQ(pointer.readLock()->value, 2);
}

TEST(SharedCopyOnWrite, assignmentReseatsOnlyThePointerAssignedTo)
{
    auto first = safe::makeSharedCopyOnWrite<Tracked>(1);
    auto second = safe::makeSharedCopyOnWrite<Tracked>(2);
    auto alsoFirst = first;

    first = second;
    CHECK(first == second);
    CHECK(alsoFirst != first);
    CHECK_EQ(alsoFirst.readLock()->value, 1);
    CHECK_EQ(alsoFirst.useCount(), std::size_t(1));

    first = safe::makeCopyOnWrite<Tracked>(3); // a temporary: a slot of its own
    CHECK_EQ(first.readLock()->value, 3);
    CHECK_EQ(second.readLock()->value, 2);
    CHECK_EQ(second.useCount(), std::size_t(1));

    auto element = safe::makeCopyOnWrite<Tracked>(4);
    first = element; // a CopyOnWritePtr that stays: its slot
    first.writeLock()->value = 5;
    CHECK_EQ(element.readLock()->value, 5);
}

TEST(SharedCopyOnWrite, resetSwapAndNullAssignment)
{
    Tracked::resetCounters();
    auto first = safe::makeSharedCopyOnWrite<Tracked>(1);
    auto second = safe::makeSharedCopyOnWrite<Tracked>(2);
    const void *firstSlot = first.owner();

    swap(first, second);
    CHECK(second.owner() == firstSlot);
    CHECK_EQ(first.readLock()->value, 2);
    CHECK_EQ(second.readLock()->value, 1);

    first.swap(second);
    CHECK(first.owner() == firstSlot);

    auto copy = first;
    first.reset();
    CHECK(!first);
    CHECK_EQ(Tracked::alive.load(), 2); // copy still holds the slot
    copy = nullptr;
    CHECK_EQ(Tracked::alive.load(), 1);
    second.reset();
    CHECK_EQ(Tracked::alive.load(), 0);
}

TEST(SharedCopyOnWrite, comparesAndOrdersBySlot)
{
    auto first = safe::makeSharedCopyOnWrite<Derived>();
    auto second = safe::makeSharedCopyOnWrite<Derived>();
    safe::SharedCopyOnWritePtr<Base> firstAsBase = first;
    auto before = first.snapshot();

    CHECK(first == firstAsBase);
    CHECK((first <=> firstAsBase) == std::strong_ordering::equal);
    CHECK(first != second);
    CHECK((first <=> second) != std::strong_ordering::equal);

    // Cloning the object does not change who the pointer is
    const void *slot = first.owner();
    first.writeLock()->base = 9;
    CHECK(first.owner() == slot);
    CHECK(first == firstAsBase);
    CHECK_EQ(before.readLock()->base, 1);

    std::map<safe::SharedCopyOnWritePtr<Derived>, int> ordered;
    ordered[first] = 1;
    ordered[second] = 2;
    ordered[safe::SharedCopyOnWritePtr<Derived>(first)] = 3;
    CHECK_EQ(ordered.size(), std::size_t(2));
    CHECK_EQ(ordered[first], 3);
}

TEST(SharedCopyOnWrite, comparesWithTheOwnersOfTheObjectItHolds)
{
    auto source = safe::makeCopyOnWrite<Derived>();
    auto other = safe::makeCopyOnWrite<Derived>();
    auto pointer = branchOf(source);
    safe::SharedCopyOnWritePtr<Base> base = pointer;
    const safe::SharedPtr<Base> plain = source;

    CHECK(pointer == source);
    CHECK(source == pointer);
    CHECK(base == source); // whatever base class either side looks through
    CHECK(pointer == plain);
    CHECK(plain == base);
    CHECK(pointer != other);
    CHECK(other != pointer);

    // The slot moves on, the pointers it is compared with do not
    pointer.writeLock()->base = 2;
    CHECK(pointer != source);
    CHECK(base != plain);
    CHECK(pointer == pointer.snapshot());
    CHECK(pointer == base); // while two pointers of one slot stay equal

    // And so does the CopyOnWritePtr a slot belongs to
    auto element = safe::makeCopyOnWrite<Derived>();
    safe::SharedCopyOnWritePtr<Base> attached = element;
    CHECK(attached == element);
    attached.writeLock()->base = 3;
    CHECK(attached == element);
    CHECK(element == attached);

    safe::SharedCopyOnWritePtr<Derived> empty;
    CHECK(empty == safe::SharedPtr<Derived>{});
    CHECK(empty == safe::CopyOnWritePtr<Derived>{});
    CHECK(empty != source);
    CHECK(pointer != safe::SharedPtr<Derived>{});
}

// ---------------------------------------------------------------------------
// Base classes, casts, observers

TEST(SharedCopyOnWrite, aBaseViewSharesTheSlotAndClonesTheWholeObject)
{
    auto source = safe::makeCopyOnWrite<Derived>();
    auto derived = branchOf(source);
    safe::SharedCopyOnWritePtr<Base> base = derived;
    CHECK(base == derived);
    CHECK_EQ(derived.useCount(), std::size_t(2));

    base.writeLock()->base = 7; // clones a Derived, not a Base
    CHECK_EQ(derived.readLock()->base, 7);
    CHECK_EQ(derived.readLock()->derived, 2);
    CHECK_EQ(source.readLock()->base, 1);

    derived.writeLock()->derived = 8;
    auto asDerived = safe::dynamicCast<Derived>(safe::SharedPtr<Base>(base.snapshot()));
    REQUIRE(bool(asDerived));
    CHECK_EQ(asDerived.readLock()->derived, 8);

    safe::SharedCopyOnWritePtr<Base> moved = std::move(derived);
    CHECK(!derived);
    CHECK(moved == base);
    CHECK_EQ(base.useCount(), std::size_t(2));
}

TEST(SharedCopyOnWrite, aBaseViewWithMultipleInheritanceFollowsTheObjectAround)
{
    // Right does not start where Both starts, and the address has to be right
    // again every time the slot moves to another object
    auto source = safe::makeCopyOnWrite<Both>();
    auto both = branchOf(source);
    safe::SharedCopyOnWritePtr<Right> right = both;
    safe::SharedCopyOnWritePtr<Left> left = both;
    CHECK(right == left);
    CHECK_EQ(right.readLock()->right, 20);

    right.writeLock()->right = 21; // moves the slot to a clone
    CHECK_EQ(right.readLock()->right, 21);
    CHECK_EQ(both.readLock()->right, 21);
    CHECK_EQ(both.readLock()->left, 10);
    CHECK_EQ(both.readLock()->both, 30);
    CHECK_EQ(left.readLock()->left, 10);
    CHECK_EQ(source.readLock()->right, 20);

    auto before = right.snapshot();
    both.writeLock()->both = 31; // and to another one
    right.writeLock()->right = 22;
    CHECK_EQ(both.readLock()->right, 22);
    CHECK_EQ(both.readLock()->both, 31);
    CHECK_EQ(before.readLock()->right, 21);

    auto beforeAsBoth = safe::dynamicCast<Both>(safe::SharedPtr<Right>(before));
    REQUIRE(bool(beforeAsBoth));
    CHECK_EQ(beforeAsBoth.readLock()->both, 30);
}

TEST(SharedCopyOnWrite, aBaseViewOfACopyOnWritePointerOfTheCompleteType)
{
    // The slot is created through the base view, the element is a Both
    auto source = safe::makeCopyOnWrite<Both>();
    auto element = source;
    safe::SharedCopyOnWritePtr<Right> right = element;

    right.writeLock()->right = 21;
    CHECK_EQ(source.readLock()->right, 20);
    CHECK_EQ(element.readLock()->right, 21);
    CHECK_EQ(element.readLock()->both, 30);
    CHECK_EQ(element.readLock()->left, 10);

    safe::SharedCopyOnWritePtr<Both> both = element;
    CHECK(both == right);
    CHECK_EQ(both.readLock()->right, 21);
}

TEST(SharedCopyOnWrite, clonesThroughAnAbstractBase)
{
    safe::SharedCopyOnWritePtr<AbstractBase> base =
        safe::makeSharedCopyOnWrite<ConcreteDerived>();
    auto before = base.snapshot();

    base.writeLock()->base = 7;
    CHECK_EQ(base.readLock()->total(), 17);
    CHECK_EQ(before.readLock()->total(), 15);
}

TEST(SharedCopyOnWrite, castsStayOnTheSlot)
{
    auto source = safe::makeCopyOnWrite<Both>();
    auto element = source;
    safe::SharedCopyOnWritePtr<Right> right = element;

    // Sideways through the complete object, which only a dynamic cast can do
    auto both = safe::dynamicCast<Both>(right);
    REQUIRE(bool(both));
    CHECK(both == right);
    CHECK_EQ(both.readLock()->both, 30);

    auto left = safe::dynamicCast<Left>(right);
    REQUIRE(bool(left));
    CHECK_EQ(left.readLock()->left, 10);

    auto down = safe::staticCast<Both>(left);
    REQUIRE(bool(down));
    CHECK_EQ(down.readLock()->right, 20);

    safe::SharedCopyOnWritePtr<const Both> readOnly = both;
    auto writable = safe::constCast<Both>(readOnly);
    REQUIRE(bool(writable));
    CHECK(writable == right);
    CHECK_EQ(right.useCount(), std::size_t(7)); // element and six pointers

    // A write through a cast is a write through the slot: it clones, and the
    // CopyOnWritePtr the slot belongs to comes along
    both.writeLock()->both = 31;
    CHECK_EQ(element.readLock()->both, 31);
    CHECK_EQ(source.readLock()->both, 30);

    down.writeLock()->right = 21;
    writable.writeLock()->left = 11;
    CHECK_EQ(right.readLock()->right, 21);
    CHECK_EQ(left.readLock()->left, 11);
    CHECK_EQ(element.readLock()->right, 21);
    CHECK_EQ(source.readLock()->right, 20);
    CHECK_EQ(source.readLock()->left, 10);

    // A cast that cannot work is empty, and so is a cast of an empty pointer
    safe::SharedCopyOnWritePtr<Base> base = safe::makeSharedCopyOnWrite<Derived>();
    CHECK(!safe::dynamicCast<Unrelated>(base));
    CHECK(bool(safe::dynamicCast<Derived>(base)));
    CHECK(!safe::dynamicCast<Derived>(safe::SharedCopyOnWritePtr<Base>{}));
    CHECK(!safe::staticCast<Derived>(safe::SharedCopyOnWritePtr<Base>{}));
    CHECK_EQ(base.useCount(), std::size_t(1)); // and leaves nothing behind
}

TEST(SharedCopyOnWrite, castsOfAnAdoptedObjectStayInsideTheTypeItWasAdoptedAs)
{
    // An object taken over from a plain SharedPtr is cloned as the type of that
    // pointer, see the CopyOnWrite suite. Adopted as a Derived, a base view can
    // be cast back to it...
    auto derivedPlain = safe::makeShared<Derived>();
    safe::SharedCopyOnWritePtr<Base> viaDerived{safe::CopyOnWritePtr<Base>(derivedPlain)};

    auto back = safe::dynamicCast<Derived>(viaDerived);
    REQUIRE(bool(back));
    back.writeLock()->derived = 3; // clones a Derived
    CHECK_EQ(derivedPlain.readLock()->derived, 2);
    CHECK_EQ(back.readLock()->derived, 3);
    CHECK_EQ(viaDerived.readLock()->base, 1);
    CHECK(bool(safe::staticCast<Derived>(viaDerived))); // the clone clones itself

    // ... but adopted through a base pointer the clone will be a Base, and a
    // pointer to the Derived the object happens to be would point past it
    safe::SharedPtr<Base> basePlain = safe::makeShared<Derived>();
    safe::SharedCopyOnWritePtr<Base> viaBase{safe::CopyOnWritePtr<Base>(basePlain)};
    CHECK(!safe::dynamicCast<Derived>(viaBase));
    CHECK(!safe::staticCast<Derived>(viaBase));
    CHECK(bool(safe::dynamicCast<Derived>(basePlain))); // cast first, then adopt

    // What stays inside that type is fine
    safe::SharedCopyOnWritePtr<const Base> readOnly = viaBase;
    auto writable = safe::constCast<Base>(readOnly);
    REQUIRE(bool(writable));
    CHECK(bool(safe::staticCast<Base>(viaBase)));

    writable.writeLock()->base = 4; // clones a Base
    CHECK_EQ(basePlain.readLock()->base, 1);
    CHECK_EQ(viaBase.readLock()->base, 4);
    CHECK(!safe::dynamicCast<Derived>(viaBase)); // which is all there is now
}

TEST(SharedCopyOnWrite, clonesAnObjectAdoptedFromASharedPointer)
{
    // The block of a plain SharedPtr cannot clone itself, the CopyOnWritePtr in
    // between remembers how and hands that on to the slot
    auto plain = safe::makeShared<Derived>();
    safe::SharedCopyOnWritePtr<Base> pointer{safe::CopyOnWritePtr<Base>(plain)};

    pointer.writeLock()->base = 9;
    CHECK_EQ(plain.readLock()->base, 1);
    CHECK_EQ(pointer.readLock()->base, 9);

    // The clone is a whole Derived, and it can be cloned again
    auto before = pointer.snapshot();
    pointer.writeLock()->base = 10;
    auto asDerived = safe::dynamicCast<Derived>(safe::SharedPtr<Base>(before));
    REQUIRE(bool(asDerived));
    CHECK_EQ(asDerived.readLock()->base, 9);
    CHECK_EQ(asDerived.readLock()->derived, 2);
    CHECK_EQ(pointer.readLock()->base, 10);

    // A snapshot of an adopted object still knows how to clone it
    auto adopted = safe::makeShared<Tracked>(1);
    safe::SharedCopyOnWritePtr<Tracked> second{safe::CopyOnWritePtr<Tracked>(adopted)};
    auto snapshot = second.snapshot();
    snapshot.writeLock()->value = 2;
    CHECK_EQ(adopted.readLock()->value, 1);
    CHECK_EQ(second.readLock()->value, 1);
    CHECK_EQ(snapshot.readLock()->value, 2);

    // And so does a CopyOnWritePtr that stays, and a copy of it
    safe::CopyOnWritePtr<Tracked> element(adopted);
    safe::SharedCopyOnWritePtr<Tracked> handle = element;
    auto copy = element;
    copy.writeLock()->value = 3;
    handle.writeLock()->value = 4;
    CHECK_EQ(adopted.readLock()->value, 1);
    CHECK_EQ(copy.readLock()->value, 3);
    CHECK_EQ(element.readLock()->value, 4);
}

TEST(SharedCopyOnWrite, aWeakPointerFollowsTheObjectNotTheSlot)
{
    auto pointer = safe::makeSharedCopyOnWrite<Both>();
    safe::SharedCopyOnWritePtr<Right> right = pointer;
    CHECK_EQ(pointer.weakCount(), std::size_t(1));

    safe::WeakPtr<Right> observer = right;
    REQUIRE(!observer.expired());
    CHECK_EQ(observer.lock().readLock()->right, 20);
    CHECK_EQ(pointer.weakCount(), std::size_t(2));
    CHECK_EQ(right.weakCount(), std::size_t(2));

    safe::WeakPtr<Left> assigned;
    assigned = pointer;
    CHECK(assigned == observer); // both watch the one object
    CHECK_EQ(pointer.weakCount(), std::size_t(3));
    assigned.reset();

    // An observer is not an owner, the write happens in place and it sees it
    right.writeLock()->right = 21;
    CHECK_EQ(observer.lock().readLock()->right, 21);

    // Once the slot moves to a clone the observer stays behind with the old
    // object, and expires with its last owner
    auto before = pointer.snapshot();
    right.writeLock()->right = 22;
    CHECK_EQ(observer.lock().readLock()->right, 21);
    CHECK_EQ(pointer.weakCount(), std::size_t(1)); // nobody watches the clone
    before.reset();
    CHECK(observer.expired());
    CHECK_EQ(pointer.readLock()->right, 22);
}

TEST(SharedCopyOnWrite, convertsExplicitlyToAnIndependentCopyOnWritePointer)
{
    Tracked::resetCounters();
    auto pointer = safe::makeSharedCopyOnWrite<Tracked>(1);
    auto copy = pointer;

    safe::CopyOnWritePtr<Tracked> branch(pointer);
    CHECK_EQ(Tracked::copied.load(), 0);
    CHECK(pointer == branch); // one object so far

    branch.writeLock()->value = 2; // clones for itself, like a snapshot()
    CHECK_EQ(Tracked::copied.load(), 1);
    CHECK(pointer != branch);
    CHECK_EQ(pointer.readLock()->value, 1);
    CHECK_EQ(copy.readLock()->value, 1);
    CHECK_EQ(branch.readLock()->value, 2);

    // To a base class, in one step, and the clone is still the whole object
    safe::SharedCopyOnWritePtr<Both> both = safe::makeSharedCopyOnWrite<Both>();
    safe::CopyOnWritePtr<Right> asRight(both);
    asRight.writeLock()->right = 21;
    CHECK_EQ(both.readLock()->right, 20);

    auto asBoth = safe::dynamicCast<Both>(safe::SharedPtr<Right>(asRight));
    REQUIRE(bool(asBoth));
    CHECK_EQ(asBoth.readLock()->right, 21);
    CHECK_EQ(asBoth.readLock()->both, 30);

    CHECK(!safe::CopyOnWritePtr<Tracked>(safe::SharedCopyOnWritePtr<Tracked>{}));
}

TEST(SharedCopyOnWrite, sharedFromThisFollowsTheObjectNotTheSlot)
{
    struct Node : safe::EnableSharedFromThis<Node>
    {
        int value = 1;
    };

    auto pointer = safe::makeSharedCopyOnWrite<Node>();
    auto self = pointer.readLock()->sharedFromThis();
    REQUIRE(bool(self));

    // self is one more owner of the object, so the slot moves to a clone
    pointer.writeLock()->value = 2;
    CHECK_EQ(self.readLock()->value, 1);
    CHECK_EQ(pointer.readLock()->value, 2);

    // The clone hands out pointers to itself
    auto cloneSelf = pointer.readLock()->sharedFromThis();
    REQUIRE(bool(cloneSelf));
    CHECK(pointer == cloneSelf);
}

// ---------------------------------------------------------------------------
// Failures and locking

TEST(SharedCopyOnWrite, aThrowingCopyLeavesTheSlotWhereItWas)
{
    auto source = safe::makeCopyOnWrite<ThrowsOnCopy>();
    auto pointer = branchOf(source);

    ThrowsOnCopy::armed = true;
    bool thrown = false;
    try {
        pointer.writeLock()->value = 2;
    } catch (const std::runtime_error &) {
        thrown = true;
    }
    ThrowsOnCopy::armed = false;

    CHECK(thrown);
    CHECK(pointer == source);
    CHECK_EQ(pointer.useCount(), std::size_t(1));
    CHECK_EQ(source.useCount(), std::size_t(2)); // no reference went missing

    pointer.writeLock()->value = 2; // and the next write works
    CHECK_EQ(pointer.readLock()->value, 2);
    CHECK_EQ(source.readLock()->value, 1);
}

TEST(SharedCopyOnWrite, takesExactlyOneLockPerGuard)
{
    auto pointer = safe::makeSharedCopyOnWrite<Tracked, InstrumentedMutex>(1);
    auto copy = pointer;

    InstrumentedMutex::resetCounters();
    copy.writeLock()->value = 2;
    CHECK_EQ(InstrumentedMutex::locks.load(), 1);
    CHECK_EQ(InstrumentedMutex::unlocks.load(), 1);
    CHECK_EQ(InstrumentedMutex::sharedLocks.load(), 0);

    InstrumentedMutex::resetCounters();
    CHECK_EQ(pointer.readLock()->value, 2);
    CHECK_EQ(InstrumentedMutex::sharedLocks.load(), 1);
    CHECK_EQ(InstrumentedMutex::sharedUnlocks.load(), 1);
    CHECK_EQ(InstrumentedMutex::locks.load(), 0);

    // The write that clones is the exception. It queues behind any other writer
    // wanting to clone, and reads the shared object under its shared lock.
    auto snapshot = pointer.snapshot();
    InstrumentedMutex::resetCounters();
    pointer.writeLock()->value = 3;
    CHECK_EQ(InstrumentedMutex::sharedLocks.load(), 1);
    CHECK_EQ(InstrumentedMutex::sharedUnlocks.load(), 1);
    CHECK_EQ(InstrumentedMutex::locks.load(), 2);
    CHECK_EQ(InstrumentedMutex::unlocks.load(), 2);
    CHECK_EQ(snapshot.readLock()->value, 2);

    // And only that one
    InstrumentedMutex::resetCounters();
    pointer.writeLock()->value = 4;
    CHECK_EQ(InstrumentedMutex::locks.load(), 1);
    CHECK_EQ(InstrumentedMutex::sharedLocks.load(), 0);

    // The same through the CopyOnWritePtr a slot belongs to
    auto element = safe::makeCopyOnWrite<Tracked, InstrumentedMutex>(1);
    safe::SharedCopyOnWritePtr<Tracked, InstrumentedMutex> handle = element;
    InstrumentedMutex::resetCounters();
    element.writeLock()->value = 2;
    CHECK_EQ(element.readLock()->value, 2);
    CHECK_EQ(InstrumentedMutex::locks.load(), 1);
    CHECK_EQ(InstrumentedMutex::sharedLocks.load(), 1);
}
