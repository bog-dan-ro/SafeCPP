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

// Note: only CHECK may be used inside the worker threads, REQUIRE throws.

#include "TestHarness.h"
#include "TestTypes.h"

#include <algorithm>
#include <atomic>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace tst;

namespace {

const int threadCount = int(std::clamp(std::thread::hardware_concurrency(), 2u, 8u));
constexpr int iterations = 500;
constexpr int rounds = 200;

// Rendezvous point: every worker registers and then spins, so they are all
// released at the same instant and really do collide
class SpinGate
{
public:
    void wait()
    {
        m_ready.fetch_add(1, std::memory_order_release);
        while (!m_go.load(std::memory_order_acquire)) {}
    }

    void releaseWhenReady(int workers)
    {
        while (m_ready.load(std::memory_order_acquire) != workers) {}
        m_go.store(true, std::memory_order_release);
    }

private:
    std::atomic<int> m_ready{0};
    std::atomic<bool> m_go{false};
};

// A slot of its own that shares the object of source, the way a copy of source
// would. Made from source itself the pointer would share the slot of source.
template<typename T, typename M>
safe::SharedCopyOnWritePtr<T, M> branchOf(const safe::CopyOnWritePtr<T, M> &source)
{
    return safe::SharedCopyOnWritePtr<T, M>(safe::CopyOnWritePtr<T, M>(source));
}

} // namespace

TEST(Threading, concurrentCopiesKeepTheReferenceCountConsistent)
{
    Tracked::resetCounters();
    {
        auto owner = safe::makeShared<Tracked>(1);
        {
            std::vector<std::jthread> threads;
            for (int i = 0; i < threadCount; ++i) {
                threads.emplace_back([owner] {
                    for (int n = 0; n < iterations; ++n) {
                        auto copy = owner;
                        auto second = copy;
                        auto moved = std::move(second);
                        CHECK(moved.owner() == copy.owner());
                    }
                });
            }
        }
        CHECK_EQ(sharedCount(owner), size_t(1));
        CHECK_EQ(weakCount(owner), size_t(1));
        CHECK_EQ(Tracked::alive.load(), 1);
    }
    CHECK_EQ(Tracked::alive.load(), 0);
}

TEST(Threading, uniqueLockSerializesWriters)
{
    auto counter = safe::makeShared<Tracked>(0);
    {
        std::vector<std::jthread> threads;
        for (int i = 0; i < threadCount; ++i) {
            threads.emplace_back([pointer = counter]() mutable {
                for (int n = 0; n < iterations; ++n)
                    ++pointer.writeLock()->value;
            });
        }
    }
    CHECK_EQ(counter.readLock()->value, threadCount * iterations);
}

TEST(Threading, sharedLockNeverSeesAHalfWrittenObject)
{
    auto data = safe::makeShared<Tracked>(0, "0");
    std::atomic<bool> writing{true};
    {
        std::vector<std::jthread> readers;
        for (int i = 0; i < threadCount; ++i) {
            readers.emplace_back([pointer = data, &writing] {
                // Bounded, and yielding, so the readers cannot starve the writer
                for (int n = 0; n < iterations && writing.load(std::memory_order_relaxed); ++n) {
                    {
                        auto guard = pointer.readLock();
                        // The writer updates both fields inside one critical section
                        CHECK_EQ(guard->tag, std::to_string(guard->value));
                    }
                    std::this_thread::yield();
                }
            });
        }

        for (int n = 1; n <= iterations; ++n) {
            auto guard = data.writeLock();
            guard->value = n;
            guard->tag = std::to_string(n);
        }
        writing.store(false, std::memory_order_relaxed);
    }
    CHECK_EQ(data.readLock()->value, iterations);
}

TEST(Threading, weakLockRacesWithTheLastRelease)
{
    Tracked::resetCounters();
    std::atomic<int> locked{0};
    std::atomic<int> expired{0};

    for (int round = 0; round < rounds; ++round) {
        auto owner = safe::makeShared<Tracked>(round);
        safe::WeakPtr<Tracked> weak = owner;

        // Both threads spin on the same gate, so they really do collide
        SpinGate gate;
        std::jthread releaser([&owner, &gate] {
            gate.wait();
            owner = safe::SharedPtr<Tracked>{};
        });
        std::jthread locker([&weak, &gate, &locked, &expired, round] {
            gate.wait();
            if (auto pointer = weak.lock()) {
                ++locked;
                // Winning the race must hand out the real object, never a dangling one
                CHECK_EQ(pointer.readLock()->value, round);
            } else {
                ++expired;
            }
        });
        gate.releaseWhenReady(2);
    }

    CHECK_EQ(Tracked::alive.load(), 0);
    CHECK_EQ(locked.load() + expired.load(), rounds);
    std::println("    (weak lock won {} of {} races)", locked.load(), rounds);
}

TEST(Threading, copyOnWriteDetachesSafelyWhenBothOwnersWriteAtOnce)
{
    Tracked::resetCounters();
    for (int round = 0; round < rounds; ++round) {
        auto first = safe::makeCopyOnWrite<Tracked>(1);
        auto second = first;

        // Both owners detach at the same time: whoever releases the shared block
        // last must not pull the mutex out from under the other one
        SpinGate gate;
        {
            std::jthread writer([&second, &gate] {
                gate.wait();
                second.writeLock()->value = 2;
            });
            std::jthread otherWriter([&first, &gate] {
                gate.wait();
                first.writeLock()->value = 3;
            });
            gate.releaseWhenReady(2);
        }

        CHECK_EQ(first.readLock()->value, 3);
        CHECK_EQ(second.readLock()->value, 2);
        CHECK_EQ(sharedCount(first), size_t(1));
        CHECK_EQ(sharedCount(second), size_t(1));
    }
    CHECK_EQ(Tracked::alive.load(), 0);
}

TEST(Threading, castsAndWeakLocksRunConcurrently)
{
    auto owner = safe::makeShared<Both>();
    {
        std::vector<std::jthread> threads;
        for (int i = 0; i < threadCount; ++i) {
            threads.emplace_back([owner, i]() mutable {
                for (int n = 0; n < iterations; ++n) {
                    safe::SharedPtr<Left> left = owner;
                    safe::WeakPtr<Right> weakRight = safe::dynamicCast<Right>(left);

                    auto back = safe::dynamicCast<Both>(left);
                    CHECK(bool(back));
                    if (i % 2 == 0) {
                        back.writeLock()->both = n;
                    } else {
                        CHECK_EQ(back.readLock()->left, 10);
                    }

                    auto right = weakRight.lock();
                    CHECK(bool(right));
                    if (right)
                        CHECK_EQ(right.readLock()->right, 20);
                }
            });
        }
    }
    CHECK_EQ(sharedCount(owner), size_t(1));
    CHECK_EQ(weakCount(owner), size_t(1));
    CHECK_EQ(owner.readLock()->right, 20);
}

TEST(Threading, lockAllSurvivesOppositeArgumentOrders)
{
    // Without a global acquisition order this deadlocks almost immediately
    auto first = safe::makeShared<Tracked>(0);
    auto second = safe::makeShared<Tracked>(0);
    {
        std::vector<std::jthread> threads;
        for (int i = 0; i < threadCount; ++i) {
            threads.emplace_back([first, second, i]() mutable {
                for (int n = 0; n < iterations; ++n) {
                    if (i % 2 == 0) {
                        auto guard = safe::lockAll(first, second);
                        ++guard.get<0>().value;
                        ++guard.get<1>().value;
                    } else {
                        auto guard = safe::lockAll(second, first);
                        ++guard.get<0>().value;
                        ++guard.get<1>().value;
                    }
                }
            });
        }
    }
    CHECK_EQ(first.readLock()->value, threadCount * iterations);
    CHECK_EQ(second.readLock()->value, threadCount * iterations);
}

TEST(Threading, sharedFromThisIsSafeUnderContention)
{
    struct Counter : safe::EnableSharedFromThis<Counter>
    {
        int value = 0;
    };

    auto owner = safe::makeShared<Counter>();
    {
        std::vector<std::jthread> threads;
        for (int i = 0; i < threadCount; ++i) {
            threads.emplace_back([owner]() mutable {
                for (int n = 0; n < iterations; ++n) {
                    auto self = owner.readLock()->sharedFromThis();
                    CHECK(bool(self));
                    if (self)
                        ++self.writeLock()->value;
                }
            });
        }
    }
    CHECK_EQ(owner.readLock()->value, threadCount * iterations);
    CHECK_EQ(owner.useCount(), size_t(1));
}

TEST(Threading, sharedCopyOnWriteHandlesKeepTheSlotCountConsistent)
{
    Tracked::resetCounters();
    {
        auto owner = safe::makeSharedCopyOnWrite<Tracked>(1);
        {
            std::vector<std::jthread> threads;
            for (int i = 0; i < threadCount; ++i) {
                threads.emplace_back([owner] {
                    for (int n = 0; n < iterations; ++n) {
                        auto copy = owner;
                        auto second = copy;
                        auto moved = std::move(second);
                        CHECK(moved.owner() == copy.owner());
                        CHECK_EQ(moved.readLock()->value, 1);
                    }
                });
            }
        }
        CHECK_EQ(owner.useCount(), size_t(1));
        CHECK_EQ(Tracked::alive.load(), 1);
        CHECK_EQ(Tracked::copied.load(), 0);
    }
    CHECK_EQ(Tracked::alive.load(), 0);
}

TEST(Threading, sharedCopyOnWriteNeverClonesBecauseOfItsOwnPointers)
{
    // Every thread holds a guard, or waits for one, at some point. None of that
    // may look like somebody else owning the object, or each contended write
    // would copy it.
    Tracked::resetCounters();
    auto counter = safe::makeSharedCopyOnWrite<Tracked>(0, "0");
    {
        std::vector<std::jthread> threads;
        for (int i = 0; i < threadCount; ++i) {
            threads.emplace_back([pointer = counter] {
                for (int n = 0; n < iterations; ++n) {
                    {
                        auto guard = pointer.writeLock();
                        ++guard->value;
                        guard->tag = std::to_string(guard->value);
                    }
                    auto guard = pointer.readLock();
                    CHECK_EQ(guard->tag, std::to_string(guard->value));
                }
            });
        }
    }
    CHECK_EQ(counter.readLock()->value, threadCount * iterations);
    CHECK_EQ(Tracked::copied.load(), 0);
    CHECK_EQ(Tracked::alive.load(), 1);
    CHECK_EQ(counter.useCount(), size_t(1));
}

TEST(Threading, sharedCopyOnWriteLeavesTheSourceAloneWhenManyThreadsWrite)
{
    Tracked::resetCounters();
    {
        auto source = safe::makeCopyOnWrite<Tracked>(0);
        auto slot = branchOf(source);
        {
            std::vector<std::jthread> threads;
            for (int i = 0; i < threadCount; ++i) {
                threads.emplace_back([pointer = slot] {
                    for (int n = 0; n < iterations; ++n)
                        ++pointer.writeLock()->value;
                });
            }
        }
        CHECK_EQ(slot.readLock()->value, threadCount * iterations);
        CHECK_EQ(source.readLock()->value, 0);
        CHECK_EQ(Tracked::alive.load(), 2);
        CHECK_EQ(Tracked::copied.load(), 1);
    }
    CHECK_EQ(Tracked::alive.load(), 0);
}

TEST(Threading, sharedCopyOnWriteClonesOnceHoweverManyThreadsRaceForTheFirstWrite)
{
    Tracked::resetCounters();
    constexpr int clonedRounds = 50;
    for (int round = 0; round < clonedRounds; ++round) {
        auto source = safe::makeCopyOnWrite<Tracked>(0);
        auto slot = branchOf(source);

        // Released at the same instant, every thread finds the object shared
        SpinGate gate;
        {
            std::vector<std::jthread> threads;
            for (int i = 0; i < threadCount; ++i) {
                threads.emplace_back([pointer = slot, &gate] {
                    gate.wait();
                    for (int n = 0; n < 50; ++n) {
                        if (n % 2 == 0)
                            CHECK(pointer.readLock()->value >= 0);
                        ++pointer.writeLock()->value;
                    }
                });
            }
            gate.releaseWhenReady(threadCount);
        }

        CHECK_EQ(source.readLock()->value, 0);
        CHECK_EQ(slot.readLock()->value, threadCount * 50);
        CHECK(slot.snapshot().owner() != source.owner());
    }
    // One clone per round, not one per thread that wanted to write first
    CHECK_EQ(Tracked::copied.load(), clonedRounds);
    CHECK_EQ(Tracked::alive.load(), 0);
}

TEST(Threading, sharedCopyOnWriteKeepsBothWritesWhenTheyRaceForTheFirstOne)
{
    Tracked::resetCounters();
    for (int round = 0; round < rounds; ++round) {
        auto source = safe::makeCopyOnWrite<Tracked>(1);
        auto slot = branchOf(source);

        // Both find the object shared. Only one of them clones it, and the
        // write of the other one must land in that clone too.
        SpinGate gate;
        {
            std::jthread writer([pointer = slot, &gate] {
                gate.wait();
                pointer.writeLock()->value += 10;
            });
            std::jthread otherWriter([pointer = slot, &gate] {
                gate.wait();
                pointer.writeLock()->value += 100;
            });
            gate.releaseWhenReady(2);
        }

        CHECK_EQ(slot.readLock()->value, 111);
        CHECK_EQ(source.readLock()->value, 1);
        CHECK_EQ(slot.useCount(), size_t(1));
        CHECK_EQ(sharedCount(source), size_t(1));
    }
    CHECK_EQ(Tracked::alive.load(), 0);
}

TEST(Threading, sharedCopyOnWriteSnapshotsNeverChange)
{
    // Writers that find the object unshared write in place. A snapshot taken at
    // that very moment must still never see a later write.
    Tracked::resetCounters();
    {
        auto slot = safe::makeSharedCopyOnWrite<Tracked>(0, "0");
        const int writers = std::max(1, threadCount / 2);
        std::atomic<int> writersLeft{writers};
        {
            std::vector<std::jthread> threads;
            for (int i = 0; i < writers; ++i) {
                threads.emplace_back([pointer = slot, &writersLeft] {
                    for (int n = 0; n < iterations; ++n) {
                        auto guard = pointer.writeLock();
                        ++guard->value;
                        guard->tag = std::to_string(guard->value);
                    }
                    writersLeft.fetch_sub(1, std::memory_order_relaxed);
                });
            }
            for (int i = writers; i < threadCount; ++i) {
                threads.emplace_back([pointer = slot, &writersLeft] {
                    int last = 0;
                    while (writersLeft.load(std::memory_order_relaxed) > 0) {
                        auto snapshot = pointer.snapshot();
                        int value = 0;
                        {
                            auto guard = snapshot.readLock();
                            value = guard->value;
                            CHECK_EQ(guard->tag, std::to_string(value));
                        }
                        CHECK(value >= last); // the slot only ever moves forward
                        last = value;

                        std::this_thread::yield();
                        CHECK_EQ(snapshot.readLock()->value, value);
                    }
                });
            }
        }
        CHECK_EQ(slot.readLock()->value, writers * iterations);
        CHECK_EQ(slot.useCount(), size_t(1));
        CHECK_EQ(Tracked::alive.load(), 1);
    }
    CHECK_EQ(Tracked::alive.load(), 0);
}

TEST(Threading, sharedCopyOnWriteReadersNeverSeeAHalfWrittenObject)
{
    auto source = safe::makeCopyOnWrite<Tracked>(0, "0");
    auto data = branchOf(source);
    std::atomic<bool> writing{true};
    {
        std::vector<std::jthread> readers;
        for (int i = 0; i < threadCount; ++i) {
            readers.emplace_back([pointer = data, &writing] {
                int last = 0;
                for (int n = 0; n < iterations && writing.load(std::memory_order_relaxed); ++n) {
                    {
                        auto guard = pointer.readLock();
                        CHECK_EQ(guard->tag, std::to_string(guard->value));
                        CHECK(guard->value >= last);
                        last = guard->value;
                    }
                    std::this_thread::yield();
                }
            });
        }

        // The first write moves the slot to a clone while the readers are at it
        for (int n = 1; n <= iterations; ++n) {
            auto guard = data.writeLock();
            guard->value = n;
            guard->tag = std::to_string(n);
        }
        writing.store(false, std::memory_order_relaxed);
    }
    CHECK_EQ(data.readLock()->value, iterations);
    CHECK_EQ(source.readLock()->value, 0);
}

TEST(Threading, sharedCopyOnWriteBaseViewsFollowTheSlotWhileItMoves)
{
    auto slot = safe::makeSharedCopyOnWrite<Both>();
    int writers = 0;
    {
        std::vector<std::jthread> threads;
        for (int i = 0; i < threadCount; ++i) {
            writers += i % 3 == 0;
            threads.emplace_back([pointer = slot, i] {
                for (int n = 0; n < iterations; ++n) {
                    if (i % 3 == 0) {
                        // Converting asks the live object where its Right is
                        safe::SharedCopyOnWritePtr<Right> right = pointer;
                        ++right.writeLock()->right;
                    } else if (i % 3 == 1) {
                        // Makes the next write clone, so the slot keeps moving
                        auto snapshot = pointer.snapshot();
                        CHECK_EQ(snapshot.readLock()->left, 10);
                    } else {
                        safe::SharedCopyOnWritePtr<Left> left = pointer;
                        CHECK_EQ(left.readLock()->left, 10);
                        CHECK_EQ(pointer.readLock()->both, 30);
                    }
                }
            });
        }
    }
    CHECK_EQ(slot.readLock()->right, 20 + writers * iterations);
    CHECK_EQ(slot.readLock()->left, 10);
    CHECK_EQ(slot.useCount(), size_t(1));
}

TEST(Threading, sharedCopyOnWriteWorksWithASpinLock)
{
    Tracked::resetCounters();
    auto source = safe::makeCopyOnWrite<Tracked, safe::SharedSpinLock>(0, "0");
    auto slot = branchOf(source);
    {
        std::vector<std::jthread> threads;
        for (int i = 0; i < threadCount; ++i) {
            threads.emplace_back([pointer = slot] {
                for (int n = 0; n < iterations; ++n) {
                    {
                        auto guard = pointer.writeLock();
                        ++guard->value;
                        guard->tag = std::to_string(guard->value);
                    }
                    auto guard = pointer.readLock();
                    CHECK_EQ(guard->tag, std::to_string(guard->value));
                }
            });
        }
    }
    CHECK_EQ(slot.readLock()->value, threadCount * iterations);
    CHECK_EQ(source.readLock()->value, 0);
    CHECK_EQ(Tracked::copied.load(), 1);
}

TEST(Threading, sharedCopyOnWriteCastsAndObserversRaceWithTheSlotMovingOn)
{
    // Casts stay on the slot, observers and branches take the object it holds at
    // that moment. The writers keep replacing that object, and the branches keep
    // making the next write clone.
    auto slot = safe::makeSharedCopyOnWrite<Both>();
    int writers = 0;
    {
        std::vector<std::jthread> threads;
        for (int i = 0; i < threadCount; ++i) {
            writers += i % 4 == 0;
            threads.emplace_back([pointer = slot, i] {
                safe::SharedCopyOnWritePtr<Right> right = pointer;
                for (int n = 0; n < iterations; ++n) {
                    switch (i % 4) {
                    case 0:
                        ++right.writeLock()->right;
                        break;
                    case 1: {
                        auto both = safe::dynamicCast<Both>(right);
                        CHECK(bool(both));
                        if (both)
                            CHECK_EQ(both.readLock()->left, 10);
                        break;
                    }
                    case 2: {
                        safe::WeakPtr<Right> observer = pointer;
                        if (auto locked = observer.lock())
                            CHECK(locked.readLock()->right >= 20);
                        CHECK(pointer.weakCount() >= 1);
                        break;
                    }
                    default: {
                        safe::CopyOnWritePtr<Left> branch(pointer);
                        CHECK_EQ(branch.readLock()->left, 10);
                        CHECK(bool(safe::dynamicCast<Both>(safe::SharedPtr<Left>(branch))));
                        // Whether they still match is anybody's guess by now,
                        // asking must be safe
                        (void)(pointer == branch);
                        break;
                    }
                    }
                }
            });
        }
    }
    CHECK_EQ(slot.readLock()->right, 20 + writers * iterations);
    CHECK_EQ(slot.readLock()->both, 30);
    CHECK_EQ(slot.useCount(), size_t(1));
    CHECK_EQ(slot.weakCount(), size_t(1));
}

TEST(Threading, everyThreadGetsTheSameSlotOfOneCopyOnWritePointer)
{
    Tracked::resetCounters();
    for (int round = 0; round < rounds; ++round) {
        auto original = safe::makeCopyOnWrite<Tracked>(1);
        const auto element = original; // only ever read below
        std::vector<safe::SharedCopyOnWritePtr<Tracked>> handles(threadCount);

        // What several threads calling the same accessor at the same instant do.
        // One of them creates the slot, the others must end up on that one.
        SpinGate gate;
        {
            std::vector<std::jthread> threads;
            for (int i = 0; i < threadCount; ++i) {
                threads.emplace_back([&element, &handle = handles[i], &gate] {
                    gate.wait();
                    handle = element;
                });
            }
            gate.releaseWhenReady(threadCount);
        }

        for (const auto &handle : handles)
            CHECK(handle == handles[0]);
        CHECK_EQ(handles[0].useCount(), size_t(threadCount) + 1);

        handles[0].writeLock()->value = 2;
        CHECK_EQ(handles[threadCount - 1].readLock()->value, 2);
        CHECK_EQ(element.readLock()->value, 2);
        CHECK_EQ(original.readLock()->value, 1);
    }
    CHECK_EQ(Tracked::alive.load(), 0);
}

TEST(Threading, aContainerIsUsedWhileItsElementsAreWrittenThroughSharedPointers)
{
    constexpr int elements = 4;
    Tracked::resetCounters();
    {
        std::vector<safe::CopyOnWritePtr<Tracked>> original;
        for (int i = 0; i < elements; ++i)
            original.push_back(safe::makeCopyOnWrite<Tracked>(0, "0"));
        const auto working = original; // every thread only reads the container itself

        const int writers = std::max(1, threadCount / 2);
        std::atomic<int> writersLeft{writers};
        {
            std::vector<std::jthread> threads;
            for (int i = 0; i < writers; ++i) {
                threads.emplace_back([&working, &writersLeft] {
                    for (int n = 0; n < iterations; ++n) {
                        // What a function returning an element by value does
                        safe::SharedCopyOnWritePtr<Tracked> item = working[n % elements];
                        auto guard = item.writeLock();
                        ++guard->value;
                        guard->tag = std::to_string(guard->value);
                    }
                    writersLeft.fetch_sub(1, std::memory_order_relaxed);
                });
            }
            for (int i = writers; i < threadCount; ++i) {
                threads.emplace_back([&working, &original, &writersLeft, i] {
                    int last = 0;
                    for (int n = 0; writersLeft.load(std::memory_order_relaxed) > 0; ++n) {
                        const auto &element = working[n % elements];
                        switch (i % 3) {
                        case 0: { // the element itself
                            auto guard = element.readLock();
                            CHECK_EQ(guard->tag, std::to_string(guard->value));
                            break;
                        }
                        case 1: { // a copy of the container is a branch and never changes
                            const auto branch = working;
                            const int value = branch[0].readLock()->value;
                            CHECK(value >= last);
                            last = value;
                            std::this_thread::yield();
                            CHECK_EQ(branch[0].readLock()->value, value);
                            break;
                        }
                        default: { // and whatever else a pointer can be asked
                            safe::SharedPtr<Tracked> plain = element;
                            const int value = plain.readLock()->value;
                            CHECK_EQ(plain.readLock()->tag, std::to_string(value));
                            safe::WeakPtr<Tracked> observer = element;
                            CHECK(bool(element));
                            CHECK(element.useCount() >= 1);
                            CHECK(element != nullptr);
                            (void)(element == original[n % elements]);
                            break;
                        }
                        }
                    }
                });
            }
        }

        int total = 0;
        for (const auto &element : working)
            total += element.readLock()->value;
        CHECK_EQ(total, writers * iterations);
        for (const auto &element : original)
            CHECK_EQ(element.readLock()->value, 0);
    }
    CHECK_EQ(Tracked::alive.load(), 0);
}

TEST(Threading, aContainerGrowsWhileItsElementsAreWrittenThroughSharedPointers)
{
    Tracked::resetCounters();
    {
        std::vector<safe::CopyOnWritePtr<Tracked>> elements;
        elements.push_back(safe::makeCopyOnWrite<Tracked>(0));
        safe::SharedCopyOnWritePtr<Tracked> handle = elements[0];
        {
            std::vector<std::jthread> threads;
            for (int i = 0; i < threadCount; ++i) {
                threads.emplace_back([pointer = handle] {
                    for (int n = 0; n < iterations; ++n)
                        ++pointer.writeLock()->value;
                });
            }

            // Only this thread touches the container. Every time it grows,
            // elements[0] is moved: the slot goes from one pointer to the next
            // while the writers are using it.
            for (int n = 0; n < iterations; ++n) {
                elements.push_back(safe::makeCopyOnWrite<Tracked>(n));
                CHECK(elements[0].readLock()->value >= 0);
                if (n % 16 == 0)
                    elements.shrink_to_fit();
            }
        }
        CHECK_EQ(elements[0].readLock()->value, threadCount * iterations);
        CHECK_EQ(handle.readLock()->value, threadCount * iterations);
        CHECK(elements[0] == handle);
        // At most the one clone of a pointer that still held its own reference
        CHECK(Tracked::copied.load() <= 1);
    }
    CHECK_EQ(Tracked::alive.load(), 0);
}

TEST(Threading, sharedPointersKeepWritingWhileTheirCopyOnWritePointerGoesAway)
{
    Tracked::resetCounters();
    for (int round = 0; round < rounds; ++round) {
        std::optional<safe::CopyOnWritePtr<Tracked>> element(safe::makeCopyOnWrite<Tracked>(0));
        safe::SharedCopyOnWritePtr<Tracked> handle = *element;

        SpinGate gate;
        {
            std::jthread writer([pointer = handle, &gate] {
                gate.wait();
                for (int n = 0; n < 50; ++n)
                    ++pointer.writeLock()->value;
            });
            std::jthread owner([&element, &gate] {
                gate.wait();
                element.reset();
            });
            gate.releaseWhenReady(2);
        }

        CHECK_EQ(handle.readLock()->value, 50);
        CHECK_EQ(handle.useCount(), size_t(1));
    }
    CHECK_EQ(Tracked::alive.load(), 0);
}
