// ─────────────────────────────────────────────────────────────────────────────
// test/test_token_bucket.cpp
//
// Unit tests for TokenBucket.  No external test framework — just a main()
// that runs named test functions and reports pass/fail.  This keeps the
// dependency list minimal and makes the test logic transparent.
//
// Run:
//   cmake --build build && ./build/test_token_bucket   (Linux/macOS)
//   cmake --build build && .\build\Debug\test_token_bucket.exe  (Windows)
// ─────────────────────────────────────────────────────────────────────────────

#include "aegis/token_bucket.hpp"

#include <cassert>
#include <chrono>
#include <cmath>      // std::fabs
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>     // std::this_thread::sleep_for

// ── Minimal test harness ─────────────────────────────────────────────────────

static int  g_passed = 0;
static int  g_failed = 0;

// EXPECT: non-fatal check — logs failure but keeps running
#define EXPECT(cond)                                                        \
    do {                                                                    \
        if (!(cond)) {                                                      \
            std::cerr << "  FAIL [" << __FILE__ << ":" << __LINE__ << "] " \
                      << #cond << "\n";                                     \
            ++g_failed;                                                     \
        } else {                                                            \
            ++g_passed;                                                     \
        }                                                                   \
    } while (false)

// EXPECT_NEAR: floating-point equality within a tolerance
#define EXPECT_NEAR(a, b, tol)                                              \
    EXPECT(std::fabs((a) - (b)) < (tol))

static void run_test(const std::string& name, void(*fn)())
{
    std::cout << "[ TEST ] " << name << "\n";
    fn();
}

// ── Tests ─────────────────────────────────────────────────────────────────────

// A freshly constructed bucket should start full.
void test_starts_full()
{
    aegis::TokenBucket tb{5.0, 1.0};
    EXPECT_NEAR(tb.tokens(), 5.0, 1e-9);
}

// Each consume() call that succeeds should decrement tokens by 1.
void test_consume_decrements()
{
    aegis::TokenBucket tb{5.0, 1.0};

    EXPECT(tb.consume() == true);
    EXPECT_NEAR(tb.tokens(), 4.0, 0.01);

    EXPECT(tb.consume() == true);
    EXPECT_NEAR(tb.tokens(), 3.0, 0.01);
}

// Consuming exactly `capacity` tokens should drain the bucket.
void test_drain_to_empty()
{
    const int cap = 3;
    aegis::TokenBucket tb{static_cast<double>(cap), 1.0};

    for (int i = 0; i < cap; ++i) {
        EXPECT(tb.consume() == true);
    }
    // Bucket is now empty (or very close, due to tiny refill during loop)
    // A fresh consume must fail.
    EXPECT(tb.consume() == false);
}

// After the bucket is empty, waiting long enough should allow a new request.
// We sleep for a real 150 ms and use a 1-token/100ms refill rate, so we
// expect at least one token to be available.
//
// Note: timing-sensitive tests are inherently flaky on loaded CI machines.
// We use a generous 50 ms margin to minimise false failures.
void test_refill_after_wait()
{
    // 10 tokens/second = 1 token per 100 ms
    aegis::TokenBucket tb{1.0, 10.0};

    EXPECT(tb.consume() == true);   // drain the one token
    EXPECT(tb.consume() == false);  // bucket empty

    // Sleep slightly longer than one refill period (100 ms).
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // Should now have been refilled.
    EXPECT(tb.consume() == true);
}

// Tokens must be capped at capacity even after a long idle period.
void test_cap_at_capacity()
{
    aegis::TokenBucket tb{5.0, 100.0};  // very fast refill

    // Drain completely
    while (tb.consume()) {}

    // Sleep well over a full refill period
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Should be full (≈5), not over-full
    // We call consume() 5 times; each should succeed
    for (int i = 0; i < 5; ++i) {
        EXPECT(tb.consume() == true);
    }
    // 6th should fail (capped at 5, no significant extra time has passed)
    EXPECT(tb.consume() == false);
}

// seconds_until_token() should return 0 when tokens are available.
void test_retry_after_available()
{
    aegis::TokenBucket tb{2.0, 1.0};
    EXPECT_NEAR(tb.seconds_until_token(), 0.0, 1e-9);
}

// seconds_until_token() should return a positive duration when bucket is empty.
void test_retry_after_wait()
{
    // 2 tokens/second: one token arrives every 0.5 s
    aegis::TokenBucket tb{1.0, 2.0};
    tb.consume();   // drain

    double wait = tb.seconds_until_token();
    // We need (1.0 - ~0) / 2.0 ≈ 0.5 s.
    // Allow generous tolerance for scheduling jitter.
    EXPECT(wait > 0.0);
    EXPECT(wait <= 0.6);
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main()
{
    std::cout << "=== TokenBucket tests ===\n\n";

    run_test("starts_full",           test_starts_full);
    run_test("consume_decrements",    test_consume_decrements);
    run_test("drain_to_empty",        test_drain_to_empty);
    run_test("refill_after_wait",     test_refill_after_wait);
    run_test("cap_at_capacity",       test_cap_at_capacity);
    run_test("retry_after_available", test_retry_after_available);
    run_test("retry_after_wait",      test_retry_after_wait);

    std::cout << "\n=== Results: "
              << g_passed << " passed, "
              << g_failed << " failed ===\n";

    return g_failed == 0 ? 0 : 1;
}
