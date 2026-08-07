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
