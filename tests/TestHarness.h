#pragma once

// A minimal, dependency free test harness.
//
// Test cases register themselves at static initialization time:
//
//     TEST(SharedPtr, isEmptyByDefault)
//     {
//         safe::SharedPtr<int> p;
//         CHECK(!p);
//     }
//
// CHECK reports a failure and lets the case continue, REQUIRE aborts the case.
// CHECK is safe to call from several threads at once, REQUIRE is not: it throws,
// so only use it on the thread running the case.

#include <atomic>
#include <format>
#include <mutex>
#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace tst {

// Thrown by REQUIRE to abort the running case
struct AbortCase
{};

class Registry
{
public:
    using Function = void (*)();

    struct Case
    {
        std::string_view suite;
        std::string_view name;
        Function function;
    };

    static Registry &instance()
    {
        static Registry registry;
        return registry;
    }

    void add(Case testCase) { m_cases.push_back(testCase); }

    void reportFailure(const std::string &message)
    {
        m_caseFailures.fetch_add(1, std::memory_order_relaxed);
        const std::lock_guard lock(m_outputMutex);
        std::println("    {}", message);
    }

    // Runs every case whose suite matches filter, an empty filter runs them all.
    // Returns the number of failed cases.
    int run(std::string_view filter);

private:
    std::vector<Case> m_cases;
    std::mutex m_outputMutex;
    std::atomic<int> m_caseFailures{0};
};

struct Registrar
{
    Registrar(std::string_view suite, std::string_view name, Registry::Function function)
    {
        Registry::instance().add({suite, name, function});
    }
};

namespace detail {

inline void check(bool condition, std::string_view expression, std::string_view file, int line,
                  bool fatal)
{
    if (condition)
        return;

    Registry::instance().reportFailure(
        std::format("{}:{}: {} failed: {}", file, line, fatal ? "REQUIRE" : "CHECK", expression));
    if (fatal)
        throw AbortCase{};
}

template<typename T>
std::string describe(const T &value)
{
    if constexpr (std::formattable<const T &, char>)
        return std::format("{}", value);
    else
        return "<not formattable>";
}

template<typename A, typename B>
void checkEqual(const A &actual, const B &expected, std::string_view actualExpression,
                std::string_view expectedExpression, std::string_view file, int line, bool fatal)
{
    if (actual == expected)
        return;

    Registry::instance().reportFailure(std::format("{}:{}: {} failed: {} == {} ({} vs {})",
                                                   file,
                                                   line,
                                                   fatal ? "REQUIRE_EQ" : "CHECK_EQ",
                                                   actualExpression,
                                                   expectedExpression,
                                                   describe(actual),
                                                   describe(expected)));
    if (fatal)
        throw AbortCase{};
}

} // namespace detail

} // namespace tst

#define TST_CONCAT_INNER(a, b) a##b
#define TST_CONCAT(a, b) TST_CONCAT_INNER(a, b)

#define TEST(suite, name) \
    static void TST_CONCAT(suite, _##name)(); \
    static const ::tst::Registrar TST_CONCAT(tstRegistrar, __LINE__) \
    { \
        #suite, #name, &TST_CONCAT(suite, _##name) \
    }; \
    static void TST_CONCAT(suite, _##name)()

#define CHECK(expression) \
    ::tst::detail::check(bool(expression), #expression, __FILE__, __LINE__, false)
#define REQUIRE(expression) \
    ::tst::detail::check(bool(expression), #expression, __FILE__, __LINE__, true)

#define CHECK_EQ(actual, expected) \
    ::tst::detail::checkEqual((actual), (expected), #actual, #expected, __FILE__, __LINE__, false)
#define REQUIRE_EQ(actual, expected) \
    ::tst::detail::checkEqual((actual), (expected), #actual, #expected, __FILE__, __LINE__, true)
