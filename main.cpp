#include "SafeC++.h"
#include <print>

using namespace std;
int main()
{
    auto a = Safe::MakeCopyOnWrite<int>(101);
    println("a = {}", *a.sharedLock());
    return 0;
}
