# SafeCpp

Thread safe smart pointers for C++20. A single header, `SafeCpp.h`.

## What it solves

With `std::shared_ptr` the data and the mutex that protects it are two separate
objects, held together only by convention. Nothing stops you from touching the data
without the lock, and the compiler will never remind you:

```cpp
auto config = std::make_shared<Config>();
std::mutex mutex;                 // protects *config, allegedly

config->retries = 5;              // compiles. no lock. nobody notices.
```

`safe::SharedPtr` puts the mutex *inside* the control block, next to the object, and
then removes `operator->` from the pointer. There is no way to name the object
without going through a lock guard:

```cpp
auto config = safe::makeShared<Config>();

config->retries = 5;              // does not compile: there is no operator->
config.writeLock()->retries = 5;  // the only way in, and it takes the lock
```

Forgetting to lock stops being a code review problem and becomes a compile error.

## Requirements

A C++20 compiler and CMake 3.16+. The library itself uses nothing newer: concepts,
three way comparison and ranges. Only the demo and the test suite need C++23, for
`<print>`, and they ask for it themselves.

## Import it

```cmake
include(FetchContent)
FetchContent_Declare(SafeCpp
    GIT_REPOSITORY https://github.com/bog-dan-ro/SafeCPP
    GIT_TAG master)
FetchContent_MakeAvailable(SafeCpp)

target_link_libraries(myapp PUBLIC Safe::CPP)
```

Or install it and use `find_package(SafeCpp 1.0 REQUIRED)`, which gives you the same
`Safe::CPP` target. Either way the C++20 requirement travels with the target, so you
do not need to set `CMAKE_CXX_STANDARD` yourself, and a C++20 project stays C++20.

```cpp
#include <SafeCpp.h>
```

## Examples

### Reading and writing

`writeLock()` takes the mutex exclusively, `readLock()` takes it shared, and both
return an RAII guard that unlocks when it dies.

```cpp
auto counter = safe::makeShared<int>(0);

*counter.writeLock() += 1;                    // exclusive
std::println("{}", *counter.readLock());      // shared, concurrent readers welcome
```

### Critical sections are the guard's lifetime

Two calls are two critical sections. When several operations must be one, name the
guard:

```cpp
// WRONG: the lock is released between the two statements
if (queue.readLock()->empty() == false)
    queue.writeLock()->pop_back();             // may already be empty again

// RIGHT: one critical section
{
    auto guard = queue.writeLock();
    if (!guard->empty())
        guard->pop_back();
}
```

The guard keeps the object alive on its own, so it stays valid even if the last
pointer to the object goes away while you hold it.

### Observing without owning

```cpp
safe::WeakPtr<Config> observer = config;

if (auto locked = observer.lock())             // promote to an owner, may fail
    std::println("{}", locked.readLock()->retries);
```

### Copy on write

`CopyOnWritePtr` clones the object the first time you write to it while other owners
exist, so readers keep seeing the value they had.

```cpp
auto original = safe::makeCopyOnWrite<Config>();
auto copy = original;                          // no clone yet, both share

copy.writeLock()->retries = 9;                 // clones here

std::println("{} {}", original.readLock()->retries, copy.readLock()->retries); // 3 9
```

### Base classes and casts

Conversion to a base is implicit; `staticCast`, `dynamicCast` and `constCast` mirror
their `std::..._pointer_cast` counterparts and share the original control block.

```cpp
safe::SharedPtr<Derived> derived = safe::makeShared<Derived>();
safe::SharedPtr<Base> base = derived;          // implicit

if (auto back = safe::dynamicCast<Derived>(base))
    back.writeLock()->somethingDerived();
```

### Locking several objects at once

`lockAll` acquires in a fixed global order and ignores repeats, so two threads
locking the same objects in opposite order cannot deadlock.

```cpp
auto guard = safe::lockAll(from, to);
guard.get<0>().balance -= amount;
guard.get<1>().balance += amount;
```

### Getting a pointer back from inside the object

Inside a critical section you only hold a `T&`. Derive from `EnableSharedFromThis`
to find your way back:

```cpp
struct Session : safe::EnableSharedFromThis<Session>
{
    void enqueue(Registry &registry) { registry.add(sharedFromThis()); }
};
```

### Pointers as keys

`==`, `<=>` and `std::hash` all work on the *owner*, the object identity, never on
the value. So the pointers drop into both the ordered and the unordered containers,
and every view of the same object — a copy, a `WeakPtr`, a pointer up-cast to a base
— is the same key:

```cpp
std::unordered_set<safe::SharedPtr<Session>> live{session};
live.contains(session);                        // true for any copy of it

std::unordered_map<safe::WeakPtr<Session>, Stats> perSession;
```

A `WeakPtr` keeps its hash after the object dies, so an expired observer still finds
its entry and can be erased. `CopyOnWritePtr` is deliberately *not* hashable:
detaching moves it to a fresh control block, so a key already in a container would
silently stop matching it. Convert it to a `SharedPtr` if you want to key on the
object it owns right now.

### Bringing your own mutex

The second template parameter is any type satisfying `safe::SharedMutex`
(`lock`/`unlock`/`lock_shared`/`unlock_shared`); it defaults to
`std::shared_mutex`.

```cpp
auto pointer = safe::makeShared<Config, MyTracingMutex>();
```

## API at a glance

| | |
|---|---|
| `makeShared<T>(args…)` / `makeCopyOnWrite<T>(args…)` | the only ways to create one |
| `writeLock()` / `readLock()` | exclusive / shared guard, the only access to `T` |
| `lockAll(a, b, …)` | deadlock free multi lock, `get<I>()` per object |
| `WeakPtr::lock()` / `expired()` | promote to an owner / cheap check |
| `staticCast` / `dynamicCast` / `constCast` | share the control block |
| `useCount()` / `weakCount()` / `owner()` | owners, observers, identity |
| `reset()` / `swap()` / `==` / `<=>` | ownership comparison, ordered container friendly |
| `std::hash<SharedPtr/WeakPtr>` | hashes the owner, unordered container friendly |
| `EnableSharedFromThis<T>` | `sharedFromThis()`, `weakFromThis()` |

## Things it deliberately does not do

- **The pointer objects are not thread safe, only the pointee is.** Two threads must
  not assign to the *same* `SharedPtr` instance. Give each thread its own copy.
- **The locks are not recursive.** `*p.writeLock() = *p.readLock();` deadlocks on
  itself.
- **A reference can still escape.** `auto &ref = *p.writeLock();` leaves you holding
  a reference after the guard is gone. No C++ library can prevent that; it needs a
  borrow checker.
- Cycles leak, exactly as with `std::shared_ptr`. Break them with `WeakPtr`.
- No array support, aliasing constructor, custom deleters or allocators.

## Building and testing

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/examples/SafeCppDemo
cd build && ctest
```

The test suite is worth running under sanitizers, since this is a concurrency
primitive:

```bash
cmake -S . -B build-tsan -G Ninja -DCMAKE_CXX_FLAGS="-fsanitize=thread -g"
cmake --build build-tsan && (cd build-tsan && ctest)
```

`SAFECPP_BUILD_EXAMPLES`, `SAFECPP_BUILD_TESTS` and `SAFECPP_INSTALL` default to on
when SafeCpp is the top level project and off when it is consumed by another one, so
importing SafeCpp costs you nothing but the header.

## License

CC0 1.0 Universal. See [LICENSE](LICENSE).
