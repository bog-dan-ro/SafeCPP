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
    println("b = {}", b.sharedLock()->b);
}

int main()
{
    auto a = safe::makeCopyOnWrite<int>(101);
    auto b = a;
    *b.lock() = 10;
    println("a = {}, b = {}", *a.sharedLock(), *b.sharedLock());

    auto d = safe::makeShared<Derived>();
    printB(d);
    auto dcow = safe::makeCopyOnWrite<Derived>();
    printB(dcow);

    safe::SharedPtr<Base> base = d;
    auto derived = safe::dynamicCast<Derived>(base);
    println("dynamicCast<Derived>: d = {}", derived.sharedLock()->d);
    println("staticCast<Derived>: d = {}", safe::staticCast<Derived>(base).sharedLock()->d);

    auto notDerived = safe::dynamicCast<Derived>(safe::makeShared<Base>());
    println("failed dynamicCast is empty: {}", !notDerived);

    safe::WeakPtr<Base> weakBase = d;
    auto lockedBase = weakBase.lock();
    println("weak up-cast: b = {}", lockedBase.sharedLock()->b);
    return 0;
}
