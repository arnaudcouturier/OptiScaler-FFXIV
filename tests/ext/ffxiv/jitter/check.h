#pragma once
// Minimal test macros: a failed check prints its location and the run exits non-zero at the end.
#include <cstdio>

inline int g_failures = 0;

#define CHECK(cond) ((cond) ? (void) 0 : (std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond), ++g_failures, (void) 0))

#define TEST_RESULT(name)                                                                                              \
    (g_failures == 0 ? (std::printf("PASS: %s\n", name), 0) : (std::printf("FAILED: %s (%d checks)\n", name, g_failures), 1))
