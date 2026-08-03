#include "SafeC++.h"
#include <print>

using namespace std;

struct Base {
    virtual ~Base() = default;
    int b = 1;
};

struct Derived : Base {
    int d = 10;
};


void printB(const safe::SharedPtr<Base> &b) {
    println("b = {}", b.readLock()->b);
}

int main()
{
    auto a = safe::makeCopyOnWrite<int>(101);
    auto b = a;
    *b.writeLock() = 10;
    println("a = {}, b = {}", *a.readLock(), *b.readLock());

    auto d = safe::makeShared<Derived>();
    printB(d);
    auto dcow = safe::makeCopyOnWrite<Derived>();
    printB(dcow);

    safe::SharedPtr<Base> base = d;
    auto derived = safe::dynamicCast<Derived>(base);
    println("dynamicCast<Derived>: d = {}", derived.readLock()->d);
    println("staticCast<Derived>: d = {}", safe::staticCast<Derived>(base).readLock()->d);

    auto notDerived = safe::dynamicCast<Derived>(safe::makeShared<Base>());
    println("failed dynamicCast is empty: {}", !notDerived);

    safe::WeakPtr<Base> weakBase = d;
    auto lockedBase = weakBase.lock();
    println("weak up-cast: b = {}", lockedBase.readLock()->b);
    return 0;
}
