// Mixer SEND - Copyright (C) 2026 Covo Design
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <string>

namespace mixersend::test
{

using TestFn = void (*)();

void registerTest (const char* name, TestFn fn);
int runAll (const char* filter);

void expectTrue (bool condition, const std::string& expr, const char* file, int line);
void expectNear (double actual, double expected, double tolerance,
                 const std::string& expr, const char* file, int line);

/** Path of a file in packages/protocol/fixtures (shared with the TS tests). */
std::string fixturePath (const char* name);

} // namespace mixersend::test

#define MIXERSEND_CAT_(a, b) a##b
#define MIXERSEND_CAT(a, b) MIXERSEND_CAT_(a, b)

#define TEST(name)                                                                 \
    static void name();                                                            \
    static const bool MIXERSEND_CAT (name, _registered) =                          \
        (::mixersend::test::registerTest (#name, name), true);                     \
    static void name()

#define EXPECT(cond) ::mixersend::test::expectTrue ((cond), #cond, __FILE__, __LINE__)

#define EXPECT_MSG(cond, msg) ::mixersend::test::expectTrue ((cond), std::string (#cond) + "  [" + (msg) + "]", __FILE__, __LINE__)

#define EXPECT_NEAR(actual, expected, tol)                                         \
    ::mixersend::test::expectNear ((actual), (expected), (tol),                    \
                                   #actual " ~= " #expected, __FILE__, __LINE__)
