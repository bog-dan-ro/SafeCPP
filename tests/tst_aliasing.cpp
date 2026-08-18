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

#include "TestHarness.h"
#include "TestTypes.h"

#include <functional>
#include <utility>

using namespace tst;

namespace {

struct Pair
{
    int first = 1;
    int second = 2;
};

} // namespace

TEST(Aliasing, exposesAMemberWhileSharingTheOwner)
{
    auto owner = safe::makeShared<Pair>();
    int *member = nullptr;
    {
        auto guard = owner.writeLock();
        member = &guard->second;
    }

    safe::SharedPtr<int> alias(owner, member);
    CHECK_EQ(owner.useCount(), size_t(2));
    CHECK(alias.owner() == owner.owner());
    CHECK_EQ(*alias.readLock(), 2);

    *alias.writeLock() = 9;
    CHECK_EQ(owner.readLock()->second, 9);
}

TEST(Aliasing, moveConstructionTransfersTheOwner)
{
    auto owner = safe::makeShared<Pair>();
    int *member = nullptr;
    {
        auto guard = owner.writeLock();
        member = &guard->first;
    }
    const void *controlBlock = owner.owner();

    safe::SharedPtr<int> alias(std::move(owner), member);
    CHECK(!owner);
    CHECK(alias.owner() == controlBlock);
    CHECK_EQ(alias.useCount(), size_t(1));
    CHECK_EQ(*alias.readLock(), 1);
}

TEST(Aliasing, weakPointerPreservesTheAlias)
{
    auto owner = safe::makeShared<Pair>();
    int *member = nullptr;
    {
        auto guard = owner.writeLock();
        member = &guard->second;
    }

    safe::SharedPtr<int> alias(owner, member);
    safe::WeakPtr<int> observer = alias;
    alias.reset();

    auto locked = observer.lock();
    REQUIRE(bool(locked));
    CHECK_EQ(*locked.readLock(), 2);
    CHECK(locked.owner() == owner.owner());
}

TEST(Aliasing, guardKeepsTheManagedObjectAlive)
{
    Tracked::resetCounters();
    {
        auto owner = safe::makeShared<Tracked>(7);
        int *member = nullptr;
        {
            auto guard = owner.writeLock();
            member = &guard->value;
        }

        safe::SharedPtr<int> alias(owner, member);
        auto guard = alias.writeLock();
        owner.reset();
        alias.reset();

        CHECK_EQ(Tracked::alive.load(), 1);
        *guard = 11;
        CHECK_EQ(*guard, 11);
    }
    CHECK_EQ(Tracked::alive.load(), 0);
}

TEST(Aliasing, ownerComparisonAndHashIgnoreTheAliasAddress)
{
    auto owner = safe::makeShared<Pair>();
    int *first = nullptr;
    int *second = nullptr;
    {
        auto guard = owner.writeLock();
        first = &guard->first;
        second = &guard->second;
    }

    safe::SharedPtr<int> firstAlias(owner, first);
    safe::SharedPtr<int> secondAlias(owner, second);

    CHECK(firstAlias == secondAlias);
    CHECK_EQ(std::hash<safe::SharedPtr<int>>{}(firstAlias),
             std::hash<safe::SharedPtr<int>>{}(secondAlias));

    {
        auto guard = safe::lockAll(firstAlias, secondAlias);
        guard.get<0>() = 3;
        guard.get<1>() = 4;
    }
    CHECK_EQ(owner.readLock()->first, 3);
    CHECK_EQ(owner.readLock()->second, 4);
}

TEST(Aliasing, nullptrConstructionIsConstantInitialized)
{
    static constinit safe::SharedPtr<int> pointer{nullptr};
    CHECK(!pointer);
}
