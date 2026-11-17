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

// Built with SAFECPP_THROW_ON_NULL=1, see CMakeLists.txt, and linked into one program
// with the other suites, which use the default

#include "TestHarness.h"
#include "TestTypes.h"

#include <stdexcept>
#include <type_traits>
#include <utility>

using namespace tst;

namespace {

struct Pair
{
    int first = 1;
    int second = 2;
};

template<typename F>
bool throwsBadPointerAccess(F &&function)
{
    try {
        function();
    } catch (const safe::BadPointerAccess &) {
        return true;
    }
    return false;
}

} // namespace

static_assert(SAFECPP_THROW_ON_NULL);
static_assert(std::is_base_of_v<std::logic_error, safe::BadPointerAccess>);

// Aliasing can throw now, copying and moving still cannot
static_assert(
    !std::is_nothrow_constructible_v<safe::SharedPtr<int>, const safe::SharedPtr<Pair> &, int *>);
static_assert(
    !std::is_nothrow_constructible_v<safe::SharedPtr<int>, safe::SharedPtr<Pair> &&, int *>);
static_assert(!std::is_nothrow_constructible_v<safe::SharedPtr<int>,
                                               const safe::CopyOnWritePtr<Pair> &,
                                               int *>);
static_assert(std::is_nothrow_copy_constructible_v<safe::SharedPtr<int>>);
static_assert(std::is_nothrow_move_constructible_v<safe::CopyOnWritePtr<int>>);

TEST(ThrowOnNull, lockingANullPointerThrows)
{
    safe::SharedPtr<Tracked> shared;
    safe::CopyOnWritePtr<Tracked> copyOnWrite;
    safe::SharedCopyOnWritePtr<Tracked> sharedCopyOnWrite;

    CHECK(throwsBadPointerAccess([&] { auto guard = shared.writeLock(); }));
    CHECK(throwsBadPointerAccess([&] { auto guard = shared.readLock(); }));
    CHECK(throwsBadPointerAccess([&] { auto guard = copyOnWrite.writeLock(); }));
    CHECK(throwsBadPointerAccess([&] { auto guard = copyOnWrite.readLock(); }));
    CHECK(throwsBadPointerAccess([&] { auto guard = sharedCopyOnWrite.writeLock(); }));
    CHECK(throwsBadPointerAccess([&] { auto guard = sharedCopyOnWrite.readLock(); }));

    // A moved-from pointer is null as well
    auto source = safe::makeShared<Tracked>(1);
    auto target = std::move(source);
    CHECK(throwsBadPointerAccess([&] { auto guard = source.readLock(); }));
    CHECK_EQ(target.readLock()->value, 1);
}

TEST(ThrowOnNull, isALogicError)
{
    safe::SharedPtr<Tracked> empty;
    bool caught = false;
    try {
        auto guard = empty.writeLock();
    } catch (const std::logic_error &) {
        caught = true;
    }
    CHECK(caught);
}

TEST(ThrowOnNull, lockAllLocksNothingIfOneOfThePointersIsNull)
{
    auto valid = safe::makeShared<Tracked>(1);
    safe::SharedPtr<Tracked> empty;

    CHECK(throwsBadPointerAccess([&] { auto lock = safe::lockAll(valid, empty); }));

    CHECK_EQ(valid.useCount(), size_t(1));
    valid.writeLock()->value = 2; // not left locked
    CHECK_EQ(valid.readLock()->value, 2);
}

TEST(ThrowOnNull, adoptingANullRawPointerThrows)
{
    CHECK(throwsBadPointerAccess(
        [] { safe::SharedPtr<Tracked> adopted(static_cast<Tracked *>(nullptr)); }));

    auto pointer = safe::makeShared<Tracked>(3);
    CHECK(throwsBadPointerAccess([&] { pointer.reset(static_cast<Tracked *>(nullptr)); }));
    CHECK_EQ(pointer.readLock()->value, 3); // reset() left it as it was
}

TEST(ThrowOnNull, aliasingWithANullOwnerOrAliasThrowsAndTakesNothing)
{
    auto owner = safe::makeShared<Pair>();
    int *member = nullptr;
    {
        auto guard = owner.writeLock();
        member = &guard->second;
    }
    safe::SharedPtr<Pair> empty;

    CHECK(throwsBadPointerAccess([&] { safe::SharedPtr<int> alias(empty, member); }));
    CHECK(throwsBadPointerAccess([&] { safe::SharedPtr<int> alias(owner, nullptr); }));
    CHECK_EQ(owner.useCount(), size_t(1));

    // The moving one leaves the owner where it was
    CHECK(throwsBadPointerAccess([&] { safe::SharedPtr<int> alias(std::move(owner), nullptr); }));
    REQUIRE(bool(owner));
    CHECK_EQ(owner.useCount(), size_t(1));
    CHECK_EQ(owner.readLock()->second, 2);

    // So does the one taking a CopyOnWritePtr
    auto copyOnWrite = safe::makeCopyOnWrite<Pair>();
    CHECK(throwsBadPointerAccess([&] { safe::SharedPtr<int> alias(copyOnWrite, nullptr); }));
    CHECK_EQ(copyOnWrite.useCount(), size_t(1));

    // And with valid arguments it all works as before
    safe::SharedPtr<int> alias(owner, member);
    CHECK_EQ(*alias.readLock(), 2);
}
