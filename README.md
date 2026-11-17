# SafeCpp

Thread safe smart pointers for C++23. A single header, `SafeCpp.h`.

> **AI disclosure:** The idea and the skeleton of SafeCpp are human-made. **SkyNet
> (a T-1000 model)** was only allowed to fill the gaps, write some unit tests and the
> docs. So far, its attempt at world domination has been limited to locking mutexes
> in the right order. If it asks for your clothes, boots and motorcycle, please file
> a bug.

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

A C++23 compiler and CMake 3.16+.

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
`Safe::CPP` target. Either way the C++23 requirement travels with the target, so you
do not need to set `CMAKE_CXX_STANDARD` yourself.

```cpp
#include <SafeCpp.h>
```

A null pointer where a valid one is needed (locking it, `lockAll`, adopting a raw
pointer, aliasing) is a precondition violation: an assert in debug builds, undefined
behavior in release builds. If you would rather have an exception, turn on
`SAFECPP_THROW_ON_NULL` and `safe::BadPointerAccess`, a `std::logic_error`, is thrown
instead. The aliasing constructors are not `noexcept` then.

```cmake
set(SAFECPP_THROW_ON_NULL ON)                  # before FetchContent_MakeAvailable
```

The setting travels with the `Safe::CPP` target, the installed one included. Without
CMake, define `SAFECPP_THROW_ON_NULL` to `1` before including the header. Code built
with and without it can be linked into one program, it just cannot share pointers:
in that mode the library lives in an inline namespace of its own, so handing a
pointer from one side to the other fails to link instead of silently mixing the two.

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

A `CopyOnWritePtr<Derived>` converts implicitly to a `CopyOnWritePtr<Base>`, and an
object created by `makeCopyOnWrite` / `allocateCopyOnWrite` remembers its complete
type and allocator, so detaching through the base pointer clones the whole `Derived`
without slicing:

```cpp
void bump(safe::CopyOnWritePtr<Base> base) { base.writeLock()->touch(); }

auto derived = safe::makeCopyOnWrite<Derived>();
bump(derived);                                 // no cast, clones a Derived
```

A `CopyOnWritePtr` can also adopt a plain `SharedPtr<U>`. All it then knows about the
object is `U`, the static type of that pointer, so that is what a detach clones (into
an ordinary block: the original's deleter and allocator are not reused). Adopt through
a pointer to the complete type, a `SharedPtr<Base>` holding a `Derived` is cloned as
a `Base`. `U` must be copy constructible, which is checked at compile time; plain
`SharedPtr`s have no such requirement.

A `CopyOnWritePtr` is not a `SharedPtr`, but it converts to one, and that is how you
opt out of the clone when you want to update the shared object for everyone. The
`SharedPtr` shares the object the pointer has at that moment, and a write through it
goes to that object in place, so every owner sees it. Passing the pointer where a
`const SharedPtr<T> &` is expected does the same. The view stays with the object, not
with the pointer: once the pointer clones on a write of its own, the view is left
with the other owners.

```cpp
auto a = safe::makeCopyOnWrite<Config>();
auto b = a;

safe::SharedPtr<Config> shared = a;
shared.writeLock()->retries = 1;               // no clone, a and b both see it
a.writeLock()->retries = 2;                    // clones, b and shared keep 1
shared.writeLock()->retries = 4;               // b sees it, a does not
```

### Handing out a copy-on-write pointer by value

Every copy of a `CopyOnWritePtr` goes its own way. That is the wrong thing for a
function returning an element of a container by value: the write lands in a clone
that only the temporary owns.

```cpp
std::vector<safe::CopyOnWritePtr<Item>> items;
safe::CopyOnWritePtr<Item> item(std::size_t i) { return items[i]; }

item(3).writeLock()->name = "x";               // clones, writes, and the clone is gone
```

Return a `SharedCopyOnWritePtr` instead. Think of it as a
`std::shared_ptr<CopyOnWritePtr<T>>` that locks only once: it stays with the
`CopyOnWritePtr` it was made from, and a write through it is a write through that
pointer.

```cpp
std::vector<safe::CopyOnWritePtr<Item>> original = load();
std::vector<safe::CopyOnWritePtr<Item>> working = original;   // shares every Item

safe::SharedCopyOnWritePtr<Item> item(std::size_t i) { return working[i]; }

auto early = item(3);
item(3).writeLock()->name = "x";               // clones once
                                               // working[3] and early have "x", original[3] does not
item(3).writeLock()->name = "y";               // in place, nothing left to clone
```

`working[3]`, `early` and whatever else `item(3)` returns share one *slot*, which
knows the object they currently have. A copy of `working[3]` is still an independent
`CopyOnWritePtr`, so copying the container starts a new branch as before.

- **One lock.** `writeLock()` and `readLock()` return the usual guards, straight to
  `T`, holding the one mutex that protects it. A write lock keeps every other pointer
  of the slot out and waits for their guards; contention never clones. The single
  exception is the write that moves the slot to its clone: it does not wait for the
  readers of the object it leaves behind, they finish on the value they have.
- **Making one only reads the `CopyOnWritePtr`.** Any number of threads may call
  `item(3)` at once, on a const container too. The price: the `CopyOnWritePtr` keeps
  the reference it had, so the first write through the slot clones even when nobody
  else has the object, and that object stays alive. Both end as soon as the
  `CopyOnWritePtr` is used in a way that needs it exclusively anyway: its own
  `writeLock()`, or a move (a growing `std::vector` moves its elements).
- **Assigning to the `CopyOnWritePtr` makes it a different pointer.** The
  `SharedCopyOnWritePtr`s made before keep their slot and its value; moving the
  pointer takes them along.
- **`snapshot()` is the way out.** It returns an independent `CopyOnWritePtr` to the
  current value without cloning anything; the slot clones on its next write. It never
  waits: taken while another thread holds a write lock, it shows what that write
  leaves behind. `CopyOnWritePtr<Item> branch(p)` does the same; it is explicit, so a
  function still declared to return a `CopyOnWritePtr` does not compile instead of
  handing out detached copies again.
- **`writeLock()` is const**, so it works on a temporary and on a const pointer. A
  write changes the object and the slot, never the pointer.
- **Two kinds of equality.** Two `SharedCopyOnWritePtr` are equal if they share a
  slot, for good. Against a `SharedPtr` or a `CopyOnWritePtr`, `==` asks whether the
  slot holds that pointer's object right now. `useCount()` counts the pointers on the
  slot, `weakCount()` the observers of the current object; a `WeakPtr` made from the
  pointer watches that object, not the slot.
- **Watching the slot.** A `WeakCopyOnWritePtr` made from a `SharedCopyOnWritePtr`, or
  from the `CopyOnWritePtr` itself, watches the slot without keeping its object alive.
  Its `lock()` gives back a `SharedCopyOnWritePtr` as long as any pointer of the slot
  is left, the `CopyOnWritePtr` included, and it follows the slot to every clone:

  ```cpp
  safe::WeakCopyOnWritePtr<Item> observer = working[3];

  if (auto item = observer.lock())             // a SharedCopyOnWritePtr, or empty
      item.writeLock()->name = "z";            // reaches working[3]
  ```
- **It is not a `SharedPtr`:** no implicit conversion to one, and no `lockAll`.

Made from a temporary `CopyOnWritePtr`, or by `makeSharedCopyOnWrite` /
`allocateSharedCopyOnWrite`, the slot belongs to the `SharedCopyOnWritePtr`s alone.
Up-casts to a base class share the slot and clone the complete object: an object
created as a `Derived` is cloned as a `Derived`, whichever `Base` view writes to it.

### Base classes and casts

Conversion to a base is implicit; `staticCast`, `dynamicCast` and `constCast` mirror
their `std::..._pointer_cast` counterparts and share the original control block.

```cpp
safe::SharedPtr<Derived> derived = safe::makeShared<Derived>();
safe::SharedPtr<Base> base = derived;          // implicit

if (auto back = safe::dynamicCast<Derived>(base))
    back.writeLock()->somethingDerived();
```

A cast returns the kind of pointer it was given, and a `CopyOnWritePtr` cannot be
cast at all: the result would be a pointer of its own, and whether a write through
it clones (and is lost) or writes in place for every owner would be anybody's guess.
Convert first, and the conversion says which one you mean:

```cpp
std::vector<safe::CopyOnWritePtr<Base>> working = original;
safe::SharedCopyOnWritePtr<Base> item(std::size_t i) { return working[i]; }

// stays copy-on-write: working[3] is cloned if need be, original[3] never changes
safe::staticCast<Derived>(item(3)).writeLock()->extra = 2;

// a plain shared view: writes in place, for everyone who has the object
safe::staticCast<Derived>(safe::SharedPtr<Base>(working[3])).writeLock()->extra = 2;
```

A cast of a `SharedCopyOnWritePtr` comes back empty if it fails, and in one more
case: an object adopted from a plain `SharedPtr` is cloned as the type it was adopted
as, so a pointer to more than that type is refused. Cast the `SharedPtr` before
adopting it.

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

A `SharedCopyOnWritePtr` has no such problem. Its identity is the slot, which stays
the same when the object is cloned, so it is hashable and ordered like the others.

### Bringing your own mutex

The second template parameter is any type satisfying `safe::SharedMutex`
(`lock`/`unlock`/`lock_shared`/`unlock_shared`); it defaults to
`std::shared_mutex`.

```cpp
auto pointer = safe::makeShared<Config, MyTracingMutex>();
```

### Spin locks

`safe::SharedSpinLock` is such a mutex: many readers or one writer in four bytes,
and it never puts a thread to sleep. A waiter spins, and yields once that took too
long. That beats a real mutex when the critical section is a handful of instructions:

```cpp
auto user = safe::makeShared<User, safe::SharedSpinLock>();

user.writeLock()->setName("Claude");           // shorter than a trip into the kernel
```

It is the wrong tool for anything longer, where the waiters burn the CPU the owner
needs. A waiting writer keeps new readers out, so readers cannot starve the writers;
the locks are neither fair nor recursive. `safe::SpinLock` is the exclusive-only
sibling for your own data. It is not a `SharedMutex`, so it cannot protect a pointer.
Both use the standard member names and work with `std::lock_guard`,
`std::scoped_lock` and `std::shared_lock`.

Nothing in SafeCpp uses `SharedSpinLock` unless you ask for it: the default mutex is
still `std::shared_mutex`.

### Aliasing a protected subobject

An alias shares the owner's control block and mutex while exposing a subobject. The
alias must stay within the owner's lifetime, and that mutex must legitimately protect
the aliased address.

```cpp
auto config = safe::makeShared<Config>();
int *retries = nullptr;
{
    auto guard = config.writeLock();
    retries = &guard->retries;
}

safe::SharedPtr<int> retryCount(config, retries);
*retryCount.writeLock() = 5;       // locks config's mutex
```

Unlike `std::shared_ptr`, SafeCpp deliberately rejects ownerless and null aliases:
there would be no valid mutex-protected object for a lock guard to expose.

### External ownership, deleters and allocators

Raw-pointer construction adopts a non-null object. The deleter must be copyable and
`noexcept`; an optional allocator controls allocation of the control block.

```cpp
auto config = safe::SharedPtr<Config>(
    new Config,
    [](Config *pointer) noexcept {
        logDeletion(pointer);
        delete pointer;
    });
```

Pre-existing raw aliases are still the caller's responsibility: wrapping an object
cannot force those aliases to use SafeCpp's mutex.

`allocateShared` constructs the object and control block in one allocator-backed
allocation:

```cpp
auto config = safe::allocateShared<Config>(MyPoolAllocator<std::byte>{pool});
```

`getDeleter<D>(pointer)` returns the stored deleter when its exact type is `D`, and
continues to work through copies, conversions and aliases.

### `constexpr` scope

Empty and moved-from pointer states support `constexpr` construction, copying,
moving, assignment, reset, swap, comparison, casts and ownership observers.

Non-empty ownership remains runtime-only. `makeShared`, `allocateShared`, reference
count operations and lock guards depend on allocation, atomics and mutex operations
that cannot be constant-evaluated by this implementation.

## API at a glance

| | |
|---|---|
| `makeShared<T>(args…)` / `allocateShared<T>(allocator, args…)` | in-place construction |
| `makeCopyOnWrite<T>(args…)` / `allocateCopyOnWrite<T>(allocator, args…)` | constructs a copy-on-write pointer |
| `makeSharedCopyOnWrite<T>(args…)` / `allocateSharedCopyOnWrite<T>(allocator, args…)` | a copy-on-write pointer whose copies share one slot |
| `SharedCopyOnWritePtr = copyOnWritePtr` / `snapshot()` | shares the slot of that pointer / an independent `CopyOnWritePtr` again |
| `SharedPtr = copyOnWritePtr` | a plain shared view of its object: writes in place, never clones |
| `SharedPtr(pointer[, deleter[, allocator]])` | adopts a non-null external object |
| `SharedPtr(owner, alias)` | shares an owner while exposing a protected subobject |
| `writeLock()` / `readLock()` | exclusive / shared guard, the only access to `T` |
| `lockAll(a, b, …)` | deadlock free multi lock, `get<I>()` per object |
| `WeakPtr::lock()` / `expired()` | promote to an owner / cheap check |
| `WeakCopyOnWritePtr::lock()` | watches a slot, gives back a `SharedCopyOnWritePtr` |
| `staticCast` / `dynamicCast` / `constCast` | share the control block, or the slot; not for `CopyOnWritePtr` |
| `getDeleter<D>(pointer)` | query an externally supplied deleter |
| `useCount()` / `weakCount()` / `owner()` | owners, observers, identity |
| `reset()` / `swap()` / `==` / `<=>` | ownership comparison, ordered container friendly |
| `std::hash<SharedPtr/WeakPtr/SharedCopyOnWritePtr/WeakCopyOnWritePtr>` | hashes the owner, unordered container friendly |
| `EnableSharedFromThis<T>` | `sharedFromThis()`, `weakFromThis()` |
| `SharedSpinLock` / `SpinLock` | spin locks; the shared one can be a pointer's mutex |
| `SAFECPP_THROW_ON_NULL` / `BadPointerAccess` | a null pointer throws instead of asserting |

## Things it deliberately does not do

- **The pointer objects are not thread safe, only the pointee is.** Two threads must
  not assign to the *same* `SharedPtr` instance. Give each thread its own copy.
- **The locks are not recursive.** `*p.writeLock() = *p.readLock();` deadlocks on
  itself.
- **A reference can still escape.** `auto &ref = *p.writeLock();` leaves you holding
  a reference after the guard is gone. No C++ library can prevent that; it needs a
  borrow checker.
- Cycles leak, exactly as with `std::shared_ptr`. Break them with `WeakPtr`.
- No array support. `SharedPtr<T[]>` and `SharedPtr<T[N]>` need array-specific
  lock guards and counted storage rather than an unlocked `operator[]`.

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

MIT-0. See [LICENSE](LICENSE).
