#include "SafeC++.h"
#include <print>

using namespace std;
int main()
{
    auto a = safe::makeCopyOnWrite<int>(101);
    auto b = a;
    *b.lock() = 10;
    println("a = {}, b = {}", *a.sharedLock(), *b.sharedLock());
    return 0;
}
