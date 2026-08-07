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
