// Mixer SEND - Copyright (C) 2026 Covo Design
// SPDX-License-Identifier: AGPL-3.0-only

#include "test_framework.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace mixersend::test
{

namespace
{
    struct Entry
    {
        const char* name;
        TestFn fn;
    };

    std::vector<Entry>& registry()
    {
        static std::vector<Entry> r;
        return r;
    }

    int failuresInCurrentTest = 0;
    int totalFailures = 0;
}

void registerTest (const char* name, TestFn fn)
{
    registry().push_back ({ name, fn });
}

void expectTrue (bool condition, const std::string& expr, const char* file, int line)
{
    if (! condition)
    {
        ++failuresInCurrentTest;
        std::printf ("      FAIL %s:%d\n           %s\n", file, line, expr.c_str());
    }
}

void expectNear (double actual, double expected, double tolerance,
                 const std::string& expr, const char* file, int line)
{
    const double diff = std::abs (actual - expected);

    if (! (diff <= tolerance) || std::isnan (actual))
    {
        ++failuresInCurrentTest;
        std::printf ("      FAIL %s:%d\n           %s\n"
                     "           actual = %.9f  expected = %.9f  diff = %.9f  tol = %.9f\n",
                     file, line, expr.c_str(), actual, expected, diff, tolerance);
    }
}

std::string fixturePath (const char* name)
{
    return std::string (MIXER_SEND_FIXTURE_DIR) + "/" + name;
}

int runAll (const char* filter)
{
    int passed = 0, run = 0;

    std::printf ("Mixer SEND tests (%zu)\n\n", registry().size());

    for (const auto& e : registry())
    {
        if (filter != nullptr && std::strstr (e.name, filter) == nullptr)
            continue;

        ++run;
        failuresInCurrentTest = 0;
        const auto start = std::chrono::steady_clock::now();
        e.fn();
        const auto ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - start).count();

        if (failuresInCurrentTest == 0)
        {
            std::printf ("  ok    %-58s %8.1f ms\n", e.name, ms);
            ++passed;
        }
        else
        {
            std::printf ("  FAIL  %s  (%d assertion%s)\n",
                         e.name, failuresInCurrentTest, failuresInCurrentTest == 1 ? "" : "s");
            totalFailures += failuresInCurrentTest;
        }
    }

    std::printf ("\n%d/%d tests passed", passed, run);

    if (totalFailures > 0)
        std::printf (", %d assertion failures", totalFailures);

    std::printf ("\n");

    return totalFailures == 0 && run > 0 ? 0 : 1;
}

} // namespace mixersend::test

int main (int argc, char** argv)
{
    return mixersend::test::runAll (argc > 1 ? argv[1] : nullptr);
}
