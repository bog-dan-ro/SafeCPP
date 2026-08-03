#include "TestHarness.h"

#include <algorithm>
#include <exception>
#include <print>
#include <tuple>

namespace tst {

int Registry::run(std::string_view filter)
{
    std::ranges::sort(m_cases, [](const Case &lhs, const Case &rhs) {
        return std::tie(lhs.suite, lhs.name) < std::tie(rhs.suite, rhs.name);
    });

    int executed = 0;
    int failed = 0;
    for (const Case &testCase : m_cases) {
        if (!filter.empty() && testCase.suite != filter)
            continue;

        ++executed;
        m_caseFailures.store(0, std::memory_order_relaxed);
        std::println("[ RUN      ] {}.{}", testCase.suite, testCase.name);
        try {
            testCase.function();
        } catch (const AbortCase &) {
            // Already reported by REQUIRE
        } catch (const std::exception &error) {
            reportFailure(std::format("unexpected exception: {}", error.what()));
        } catch (...) {
            reportFailure("unexpected exception of unknown type");
        }

        if (m_caseFailures.load(std::memory_order_relaxed) == 0) {
            std::println("[       OK ] {}.{}", testCase.suite, testCase.name);
        } else {
            ++failed;
            std::println("[   FAILED ] {}.{}", testCase.suite, testCase.name);
        }
    }

    if (executed == 0) {
        std::println("no test case matched '{}'", filter);
        return 1;
    }

    std::println("{} of {} test cases passed", executed - failed, executed);
    return failed;
}

} // namespace tst

int main(int argc, char **argv)
{
    const std::string_view filter = argc > 1 ? argv[1] : "";
    return tst::Registry::instance().run(filter) == 0 ? 0 : 1;
}
