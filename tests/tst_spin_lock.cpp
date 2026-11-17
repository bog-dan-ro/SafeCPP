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
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

using namespace tst;

namespace {

const int threadCount = int(std::clamp(std::thread::hardware_concurrency(), 2u, 8u));
constexpr int iterations = 2000;

// Only the shared lock can protect the object of a pointer
static_assert(safe::SharedMutex<safe::SharedSpinLock>);
static_assert(!safe::SharedMutex<safe::SpinLock>);

static_assert(!std::is_copy_constructible_v<safe::SpinLock>);
static_assert(!std::is_copy_constructible_v<safe::SharedSpinLock>);
static_assert(sizeof(safe::SharedSpinLock) < sizeof(std::shared_mutex));

// Usable before main() runs
constinit safe::SpinLock globalSpinLock;
constinit safe::SharedSpinLock globalSharedSpinLock;

} // namespace

TEST(SpinLock, tryLockReportsWhetherItGotTheLock)
{
    safe::SpinLock lock;

    CHECK(lock.try_lock());
    CHECK(!lock.try_lock());
    lock.unlock();

    lock.lock();
    CHECK(!lock.try_lock());
    lock.unlock();
    CHECK(lock.try_lock());
    lock.unlock();

    CHECK(globalSpinLock.try_lock());
    globalSpinLock.unlock();
}

TEST(SpinLock, sharedLockAdmitsManyReadersOrOneWriter)
{
    safe::SharedSpinLock lock;

    lock.lock_shared();
    CHECK(lock.try_lock_shared()); // a second reader
    CHECK(!lock.try_lock());       // but no writer
    lock.unlock_shared();
    CHECK(!lock.try_lock()); // still one reader left
    lock.unlock_shared();

    CHECK(lock.try_lock());
    CHECK(!lock.try_lock());
    CHECK(!lock.try_lock_shared());
    lock.unlock();

    lock.lock();
    CHECK(!lock.try_lock_shared());
    lock.unlock();
    CHECK(lock.try_lock_shared());
    lock.unlock_shared();

    CHECK(globalSharedSpinLock.try_lock());
    globalSharedSpinLock.unlock();
}

TEST(SpinLock, worksWithTheStandardLockGuards)
{
    safe::SpinLock spin;
    safe::SharedSpinLock shared;
    {
        const std::lock_guard guard(spin);
        CHECK(!spin.try_lock());
    }
    {
        const std::scoped_lock guard(spin, shared);
        CHECK(!spin.try_lock());
        CHECK(!shared.try_lock_shared());
    }
    {
        std::unique_lock guard(shared, std::try_to_lock);
        CHECK(guard.owns_lock());
    }
    {
        const std::shared_lock first(shared);
        std::shared_lock second(shared, std::try_to_lock);
        CHECK(second.owns_lock());
        CHECK(!shared.try_lock());
    }
    CHECK(spin.try_lock());
    CHECK(shared.try_lock());
    spin.unlock();
    shared.unlock();
}

TEST(SpinLock, serializesItsOwners)
{
    safe::SpinLock lock;
    int counter = 0; // deliberately not atomic
    {
        std::vector<std::jthread> threads;
        for (int i = 0; i < threadCount; ++i) {
            threads.emplace_back([&lock, &counter] {
                for (int n = 0; n < iterations; ++n) {
                    const std::lock_guard guard(lock);
                    ++counter;
                }
            });
        }
    }
    CHECK_EQ(counter, threadCount * iterations);
}

TEST(SpinLock, sharedLockKeepsReadersAndWritersApart)
{
    safe::SharedSpinLock lock;
    int value = 0; // deliberately not atomic
    int mirror = 0;
    {
        std::vector<std::jthread> threads;
        for (int i = 0; i < threadCount; ++i) {
            threads.emplace_back([&lock, &value, &mirror, i] {
                for (int n = 0; n < iterations; ++n) {
                    if (i % 2 == 0) {
                        const std::lock_guard guard(lock);
                        ++value;
                        ++mirror;
                    } else {
                        const std::shared_lock guard(lock);
                        // A writer updates both inside one critical section
                        CHECK_EQ(value, mirror);
                    }
                }
            });
        }
    }
    CHECK_EQ(value, ((threadCount + 1) / 2) * iterations);
    CHECK_EQ(mirror, value);
}

TEST(SpinLock, aWaitingWriterKeepsNewReadersOut)
{
    safe::SharedSpinLock lock;
    std::atomic<bool> written{false};

    lock.lock_shared();
    {
        std::jthread writer([&lock, &written] {
            lock.lock(); // has to wait for the reader
            written.store(true, std::memory_order_relaxed);
            lock.unlock();
        });

        // Readers get in next to the first one until the writer has announced
        // itself. Without the announcement this loop would never end.
        while (lock.try_lock_shared()) {
            lock.unlock_shared();
            std::this_thread::yield();
        }
        CHECK(!written.load(std::memory_order_relaxed));
        CHECK(!lock.try_lock()); // and the announcement is not the lock itself

        lock.unlock_shared();
    }
    CHECK(written.load(std::memory_order_relaxed));

    // The announcement is gone with the writer
    CHECK(lock.try_lock_shared());
    lock.unlock_shared();
}

TEST(SpinLock, sharedSpinLockProtectsTheObjectOfAPointer)
{
    auto data = safe::makeShared<Tracked, safe::SharedSpinLock>(0, "0");
    static_assert(
        std::is_same_v<decltype(data), safe::SharedPtr<Tracked, safe::SharedSpinLock>>);
    {
        std::vector<std::jthread> threads;
        for (int i = 0; i < threadCount; ++i) {
            threads.emplace_back([pointer = data, i] {
                for (int n = 0; n < iterations; ++n) {
                    if (i % 2 == 0) {
                        auto guard = pointer.writeLock();
                        ++guard->value;
                        guard->tag = std::to_string(guard->value);
                    } else {
                        auto guard = pointer.readLock();
                        CHECK_EQ(guard->tag, std::to_string(guard->value));
                    }
                }
            });
        }
    }
    CHECK_EQ(data.readLock()->value, ((threadCount + 1) / 2) * iterations);
    CHECK_EQ(data.useCount(), std::size_t(1));
}

TEST(SpinLock, sharedSpinLockWorksWithEveryPointerType)
{
    Tracked::resetCounters();
    auto cow = safe::makeCopyOnWrite<Tracked, safe::SharedSpinLock>(1);
    auto copy = cow;
    copy.writeLock()->value = 2;
    CHECK_EQ(cow.readLock()->value, 1);
    CHECK_EQ(copy.readLock()->value, 2);

    safe::SharedCopyOnWritePtr<Tracked, safe::SharedSpinLock> slot = copy;
    auto other = slot;
    auto branch = copy;
    other.writeLock()->value = 3;
    CHECK_EQ(slot.readLock()->value, 3);
    CHECK_EQ(copy.readLock()->value, 3); // the pointer the slot belongs to
    CHECK_EQ(branch.readLock()->value, 2);
    CHECK_EQ(cow.readLock()->value, 1);

    safe::WeakPtr<Tracked, safe::SharedSpinLock> observer = copy;
    CHECK_EQ(observer.lock().readLock()->value, 3);
    CHECK_EQ(Tracked::copied.load(), 2);
}
