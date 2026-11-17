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

#include <functional>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

using namespace tst;

namespace {

template<typename P>
std::size_t hashOf(const P &pointer)
{
    return std::hash<P>{}(pointer);
}

// std::hash<P>{}(p) inside a requires expression is a hard error rather than a
// substitution failure, the standard library disables the primary template by
// deleting its special members. Ask the traits instead.
template<typename P>
inline constexpr bool hashable = std::is_default_constructible_v<std::hash<P>>
                                 && std::is_invocable_v<std::hash<P>, const P &>;

} // namespace

TEST(Hash, isProvidedForOwningAndObservingPointers)
{
    static_assert(hashable<safe::SharedPtr<Tracked>>);
    static_assert(hashable<safe::WeakPtr<Tracked>>);
    static_assert(hashable<safe::SharedPtr<Tracked, InstrumentedMutex>>);
    static_assert(hashable<safe::WeakPtr<Tracked, InstrumentedMutex>>);
    CHECK(true);
}

TEST(Hash, isStableForCopiesOfTheSamePointer)
{
    auto pointer = safe::makeShared<Tracked>(1);
    auto copy = pointer;

    CHECK_EQ(hashOf(pointer), hashOf(copy));
    CHECK_EQ(hashOf(pointer), hashOf(pointer));
}

TEST(Hash, differsForDistinctObjects)
{
    auto first = safe::makeShared<Tracked>(1);
    auto second = safe::makeShared<Tracked>(1);

    // Equal contents, different owners: hashing follows ==, not the value
    CHECK(!(first == second));
    CHECK(hashOf(first) != hashOf(second));
}

TEST(Hash, matchesTheOwnerIdentity)
{
    auto pointer = safe::makeShared<Tracked>(1);
    CHECK_EQ(hashOf(pointer), std::hash<const void *>{}(pointer.owner()));
}

TEST(Hash, isZeroLikeForEmptyPointers)
{
    safe::SharedPtr<Tracked> first;
    safe::SharedPtr<Tracked> second;
    safe::WeakPtr<Tracked> observer;

    CHECK_EQ(hashOf(first), hashOf(second));
    CHECK_EQ(hashOf(first), std::hash<const void *>{}(nullptr));
    CHECK_EQ(hashOf(observer), std::hash<const void *>{}(nullptr));
}

TEST(Hash, agreesAcrossPointerKinds)
{
    auto shared = safe::makeShared<Tracked>(1);
    const safe::WeakPtr<Tracked> observer = shared;

    // Same control block, so both views hash alike
    CHECK_EQ(hashOf(shared), hashOf(observer));
}

TEST(Hash, agreesAcrossBaseAndDerivedViews)
{
    auto derived = safe::makeShared<Derived>();
    const safe::SharedPtr<Base> base = derived;

    // Casting to a base shifts nothing here, but the identity is the block
    CHECK(base == derived);
    CHECK_EQ(hashOf(base), hashOf(derived));

    // Multiple inheritance does shift the object address, the block still matches
    auto both = safe::makeShared<Both>();
    const safe::SharedPtr<Right> right = both;
    CHECK(right == both);
    CHECK_EQ(hashOf(right), hashOf(both));
}

TEST(Hash, worksWithANonDefaultMutex)
{
    using Pointer = safe::SharedPtr<Tracked, InstrumentedMutex>;

    auto pointer = safe::makeShared<Tracked, InstrumentedMutex>(1);
    auto copy = pointer;

    std::unordered_set<Pointer> owners;
    owners.insert(pointer);
    CHECK(owners.contains(copy));
    CHECK_EQ(owners.size(), std::size_t(1));

    std::unordered_set<safe::WeakPtr<Tracked, InstrumentedMutex>> observers;
    observers.insert(safe::WeakPtr<Tracked, InstrumentedMutex>(pointer));
    CHECK(observers.contains(safe::WeakPtr<Tracked, InstrumentedMutex>(copy)));
}

TEST(Hash, dedupesCopiesInsideAnUnorderedSet)
{
    Tracked::resetCounters();

    auto first = safe::makeShared<Tracked>(1);
    auto second = safe::makeShared<Tracked>(2);
    auto absent = safe::makeShared<Tracked>(3);

    std::unordered_set<safe::SharedPtr<Tracked>> owners;
    owners.insert(first);
    owners.insert(first); // same owner, no new element
    owners.insert(second);

    CHECK_EQ(owners.size(), std::size_t(2));
    CHECK(owners.contains(first));
    CHECK(owners.contains(second));
    CHECK(!owners.contains(absent));

    // The set holds owners, so erasing it must not destroy anything early
    CHECK_EQ(sharedCount(first), std::size_t(2));
    owners.clear();
    CHECK_EQ(sharedCount(first), std::size_t(1));
    CHECK_EQ(Tracked::alive.load(), 3);
}

TEST(Hash, keepsSharedPointerKeysAlive)
{
    Tracked::resetCounters();

    std::unordered_set<safe::SharedPtr<Tracked>> owners;
    owners.insert(safe::makeShared<Tracked>(1));
    CHECK_EQ(Tracked::alive.load(), 1);

    owners.clear();
    CHECK_EQ(Tracked::alive.load(), 0);
}

TEST(Hash, findsAnExpiredWeakPointer)
{
    Tracked::resetCounters();

    auto pointer = safe::makeShared<Tracked>(1);
    safe::WeakPtr<Tracked> observer = pointer;

    std::unordered_map<safe::WeakPtr<Tracked>, int> byObserver;
    byObserver[observer] = 42;
    const std::size_t hashWhileAlive = hashOf(observer);

    pointer.reset();
    REQUIRE(observer.expired());
    CHECK_EQ(Tracked::alive.load(), 0);

    // The control block outlives the object, so the key stays valid and stable
    CHECK_EQ(hashOf(observer), hashWhileAlive);
    REQUIRE(byObserver.contains(observer));
    CHECK_EQ(byObserver[observer], 42);
}

TEST(Hash, treatsAWeakPointerCopyAsTheSameKey)
{
    auto pointer = safe::makeShared<Tracked>(1);
    safe::WeakPtr<Tracked> first = pointer;
    safe::WeakPtr<Tracked> second = first;

    CHECK(first == second);
    CHECK_EQ(hashOf(first), hashOf(second));

    std::unordered_set<safe::WeakPtr<Tracked>> observers{first, second};
    CHECK_EQ(observers.size(), std::size_t(1));
}

TEST(Hash, isNotProvidedForCopyOnWrite)
{
    // Hashing a CopyOnWritePtr is a compile error on purpose: detaching gives it
    // a new control block, so a key left in a container would go stale
    static_assert(!hashable<safe::CopyOnWritePtr<Tracked>>);
    static_assert(!hashable<safe::CopyOnWritePtr<Tracked, InstrumentedMutex>>);

    // Converting to a SharedPtr is the explicit way in, and it keys on the object
    // owned at that moment
    auto cow = safe::makeCopyOnWrite<Tracked>(1);
    auto other = cow; // a second owner, so the next write detaches
    const safe::SharedPtr<Tracked> before = cow;

    std::unordered_set<safe::SharedPtr<Tracked>> owners{before};
    cow.writeLock()->value = 2;

    const safe::SharedPtr<Tracked> after = cow;
    CHECK(hashOf(after) != hashOf(before));
    CHECK(!owners.contains(after));
    CHECK_EQ(hashOf(safe::SharedPtr<Tracked>(other)), hashOf(before));
}

TEST(Hash, comparesWeakPointersByOwner)
{
    auto first = safe::makeShared<Derived>();
    auto second = safe::makeShared<Derived>();

    safe::WeakPtr<Derived> firstObserver = first;
    safe::WeakPtr<Base> asBase = firstObserver;
    safe::WeakPtr<Derived> secondObserver = second;

    // Watching the same object through a base is still the same owner
    CHECK(firstObserver == asBase);
    CHECK_EQ(hashOf(firstObserver), hashOf(asBase));
    CHECK(!(firstObserver == secondObserver));
    CHECK((firstObserver <=> asBase) == std::strong_ordering::equal);
    CHECK((firstObserver <=> secondObserver) != std::strong_ordering::equal);

    CHECK_EQ(firstObserver.owner(), first.owner());
    safe::WeakPtr<Derived> empty;
    CHECK(empty.owner() == nullptr);
}

TEST(Hash, followsTheSlotOfASharedCopyOnWritePointer)
{
    static_assert(hashable<safe::SharedCopyOnWritePtr<Tracked>>);
    static_assert(hashable<safe::SharedCopyOnWritePtr<Tracked, InstrumentedMutex>>);

    auto source = safe::makeCopyOnWrite<Tracked>(1);
    auto element = source;
    safe::SharedCopyOnWritePtr<Tracked> pointer = element;
    safe::SharedCopyOnWritePtr<Tracked> copy = element; // the slot of element again
    safe::SharedCopyOnWritePtr<Tracked> other = source; // the slot of another pointer

    CHECK_EQ(hashOf(pointer), hashOf(copy));
    CHECK_EQ(hashOf(pointer), std::hash<const void *>{}(pointer.owner()));
    CHECK(hashOf(pointer) != hashOf(other)); // same object, different slots

    std::unordered_set<safe::SharedCopyOnWritePtr<Tracked>> slots{pointer, copy, other};
    CHECK_EQ(slots.size(), std::size_t(2));

    // Unlike a CopyOnWritePtr, the key survives the object being cloned
    const std::size_t before = hashOf(pointer);
    copy.writeLock()->value = 2;
    CHECK_EQ(hashOf(pointer), before);
    CHECK(slots.contains(pointer));
    CHECK(slots.contains(copy));

    safe::SharedCopyOnWritePtr<Tracked> empty;
    CHECK_EQ(hashOf(empty), std::hash<const void *>{}(nullptr));
}

TEST(Hash, agreesAcrossBaseAndDerivedViewsOfASlot)
{
    auto derived = safe::makeSharedCopyOnWrite<Both>();
    safe::SharedCopyOnWritePtr<Right> right = derived;

    CHECK_EQ(hashOf(derived), hashOf(right));

    std::unordered_map<safe::SharedCopyOnWritePtr<Right>, int> visits;
    visits[right] = 1;
    visits[derived] += 1; // converts, and finds the same entry
    CHECK_EQ(visits.size(), std::size_t(1));
    CHECK_EQ(visits[right], 2);
}
