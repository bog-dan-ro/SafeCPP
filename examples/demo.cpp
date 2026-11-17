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

// A short tour of SafeCpp. Each section is independent, build and run it to see
// the output. The library and this demo require C++23.

#include <SafeCpp.h>

#include <print>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace std;

namespace {

struct Config
{
    int retries = 3;
    string endpoint = "localhost";
};

struct Base
{
    virtual ~Base() = default;
    int b = 1;
};

struct Derived : Base
{
    int d = 10;
};

struct Account
{
    int balance = 100;
};

struct Session : safe::EnableSharedFromThis<Session>
{
    int id = 0;
};

// No operator-> means a plain reference is enough to read safely
void printBase(const safe::SharedPtr<Base> &pointer)
{
    println("  seen as Base: b = {}", pointer.readLock()->b);
}

void locking()
{
    println("locking");
    auto config = safe::makeShared<Config>();

    // config->retries = 5;           <- does not compile, there is no operator->
    config.writeLock()->retries = 5;  // exclusive
    println("  retries = {}", config.readLock()->retries); // shared

    // Two calls are two critical sections. Name the guard when a sequence must be
    // atomic: nobody can observe the half updated Config here.
    {
        auto guard = config.writeLock();
        guard->endpoint = "example.org";
        guard->retries = 1;
    }
    auto guard = config.readLock();
    println("  {} after {} retries", guard->endpoint, guard->retries);
}

void sharingAndObserving()
{
    println("sharing and observing");
    safe::WeakPtr<Config> observer;
    {
        auto owner = safe::makeShared<Config>();
        auto second = owner; // both own it now
        observer = owner;    // observes without owning
        println("  owners = {}, expired = {}", owner.useCount(), observer.expired());
    }
    println("  after scope: expired = {}, lock() is empty = {}",
            observer.expired(),
            !observer.lock());
}

void copyOnWrite()
{
    println("copy on write");
    auto original = safe::makeCopyOnWrite<Config>();
    auto copy = original; // shared so far, nothing cloned

    copy.writeLock()->retries = 9; // clones here, the original keeps its value

    println("  original = {}, copy = {}, still the same object = {}",
            original.readLock()->retries,
            copy.readLock()->retries,
            original == copy);
}

void sharedCopyOnWrite()
{
    println("handing out a copy-on-write pointer by value");
    vector<safe::CopyOnWritePtr<Base>> original;
    original.push_back(safe::makeCopyOnWrite<Derived>());

    // Nothing is cloned, both vectors share the Derived
    vector<safe::CopyOnWritePtr<Base>> working = original;

    // Returning a CopyOnWritePtr here would hand out independent copies, and a
    // write through one of them would be lost. A SharedCopyOnWritePtr stays with
    // working[index].
    const auto item = [&working](size_t index) -> safe::SharedCopyOnWritePtr<Base> {
        return working[index];
    };

    auto early = item(0);
    item(0).writeLock()->b = 9; // clones once, for working[0]
    item(0).writeLock()->b += 1; // in place

    println("  original = {}, working = {}, early copy = {}",
            original[0].readLock()->b,
            working[0].readLock()->b,
            early.readLock()->b);

    // A cast stays on the slot too, and the clone was a whole Derived
    safe::staticCast<Derived>(item(0)).writeLock()->d += 1;
    println("  d: original = {}, working = {}",
            safe::staticCast<Derived>(safe::SharedPtr<Base>(original[0])).readLock()->d,
            safe::staticCast<Derived>(item(0)).readLock()->d);

    // The way out: an independent CopyOnWritePtr to the current value
    auto snapshot = item(0).snapshot();
    item(0).writeLock()->b = 1;
    println("  snapshot = {}, working = {}", snapshot.readLock()->b, working[0].readLock()->b);
}

void conversionsAndCasts()
{
    println("conversions and casts");
    auto derived = safe::makeShared<Derived>();
    printBase(derived); // implicit up-cast

    auto copyOnWriteDerived = safe::makeCopyOnWrite<Derived>();
    printBase(copyOnWriteDerived); // a CopyOnWritePtr converts too

    safe::SharedPtr<Base> base = derived;
    println("  dynamicCast<Derived>: d = {}", safe::dynamicCast<Derived>(base).readLock()->d);
    println("  staticCast<Derived>:  d = {}", safe::staticCast<Derived>(base).readLock()->d);
    println("  a failed dynamicCast is empty: {}",
            !safe::dynamicCast<Derived>(safe::makeShared<Base>()));

    safe::SharedPtr<const Config> readOnly = safe::makeShared<Config>();
    println("  const view, constCast back: retries = {}",
            safe::constCast<Config>(readOnly).writeLock()->retries);
}

void severalObjectsAtOnce()
{
    println("several objects at once");
    auto from = safe::makeShared<Account>();
    auto to = safe::makeShared<Account>();

    // Always locked in the same global order, so a thread transferring the other
    // way round cannot deadlock against this one
    {
        auto guard = safe::lockAll(from, to);
        guard.get<0>().balance -= 25;
        guard.get<1>().balance += 25;
    }
    println("  from = {}, to = {}", from.readLock()->balance, to.readLock()->balance);
}

void pointerFromInsideTheObject()
{
    println("pointer from inside the object");
    auto session = safe::makeShared<Session>();
    session.writeLock()->id = 7;

    // Inside a critical section all you hold is a Session&, this gets an owner back
    auto self = session.readLock()->sharedFromThis();
    println("  id = {}, same object = {}", self.readLock()->id, self == session);
}

void manyThreads()
{
    println("many threads");
    auto counter = safe::makeShared<int>(0);
    {
        vector<jthread> threads;
        for (int i = 0; i < 8; ++i) {
            threads.emplace_back([counter]() mutable {
                for (int n = 0; n < 1000; ++n)
                    ++*counter.writeLock(); // every increment is a critical section
            });
        }
    }
    println("  8 threads x 1000 increments = {}", *counter.readLock());
}

void spinLocks()
{
    println("spin locks");
    // For critical sections of a few instructions: waiters spin instead of sleeping
    auto counter = safe::makeShared<int, safe::SharedSpinLock>(0);
    {
        vector<jthread> threads;
        for (int i = 0; i < 8; ++i) {
            threads.emplace_back([counter]() mutable {
                for (int n = 0; n < 1000; ++n)
                    ++*counter.writeLock();
            });
        }
    }
    println("  8 threads x 1000 increments = {}, the lock is {} bytes instead of {}",
            *counter.readLock(),
            sizeof(safe::SharedSpinLock),
            sizeof(shared_mutex));
}

void unorderedContainers()
{
    println("Pointers as keys:");

    auto first = safe::makeShared<string>("first");
    auto second = safe::makeShared<string>("second");
    auto absent = safe::makeShared<string>("absent");

    // Hashing is by owner, like ==, so a copy of a pointer finds the same entry
    unordered_set<safe::SharedPtr<string>> owners{first, second};
    println("  contains first {}, second {}, absent {}",
            owners.contains(first),
            owners.contains(second),
            owners.contains(absent));

    // A WeakPtr hashes the same object to the same bucket, and keeps its hash
    // once the object is gone, so an expired observer is still findable
    unordered_map<safe::WeakPtr<string>, int> visits;
    safe::WeakPtr<string> observer;
    {
        auto shortLived = safe::makeShared<string>("short lived");
        observer = shortLived;
        visits[observer] = 7;
        println("  observer and owner hash alike {}",
                hash<safe::WeakPtr<string>>{}(observer)
                    == hash<safe::SharedPtr<string>>{}(shortLived));
    }
    println("  observer expired {}, entry still found {}",
            observer.expired(),
            visits.contains(observer));
}

} // namespace

int main()
{
    locking();
    sharingAndObserving();
    copyOnWrite();
    sharedCopyOnWrite();
    conversionsAndCasts();
    severalObjectsAtOnce();
    pointerFromInsideTheObject();
    manyThreads();
    spinLocks();
    unorderedContainers();
    return 0;
}
