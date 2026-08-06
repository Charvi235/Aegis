// ─────────────────────────────────────────────────────────────────────────────
// test/test_atomic_stats.cpp
//
// Unit tests for AtomicStats.
//
// Tests cover:
//   1. Initial state — all counters start at zero.
//   2. Single-threaded increment correctness.
//   3. snapshot() returns a consistent copy.
//   4. Concurrent increments from N threads produce the exact expected total.
//      This is the most important test: if the atomics are incorrectly
//      replaced with plain ints, increments will be lost under contention
//      and the final count will be less than expected.
// ─────────────────────────────────────────────────────────────────────────────

#include "aegis/atomic_stats.hpp"

#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// ── Minimal harness (same pattern as other tests) ─────────────────────────────

static int g_passed = 0;
static int g_failed = 0;

#define EXPECT(cond)                                                         \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::cerr << "  FAIL [" << __FILE__ << ":" << __LINE__ << "] "  \
                      << #cond << "\n";                                      \
            ++g_failed;                                                      \
        } else {                                                             \
            ++g_passed;                                                      \
        }                                                                    \
    } while (false)

static void run_test(const std::string& name, void(*fn)())
{
    std::cout << "[ TEST ] " << name << "\n";
    fn();
}

// ── Tests ─────────────────────────────────────────────────────────────────────

// All counters must be zero after construction.
void test_initial_zero()
{
    aegis::AtomicStats s;
    EXPECT(s.total_requests  .load() == 0);
    EXPECT(s.allowed_requests.load() == 0);
    EXPECT(s.blocked_requests.load() == 0);
    EXPECT(s.cache_hits      .load() == 0);
    EXPECT(s.cache_misses    .load() == 0);
}

// Single-threaded increments should produce exact counts.
void test_single_thread_increment()
{
    aegis::AtomicStats s;

    for (int i = 0; i < 5; ++i)
        s.total_requests.fetch_add(1, std::memory_order_relaxed);
    for (int i = 0; i < 3; ++i)
        s.allowed_requests.fetch_add(1, std::memory_order_relaxed);
    for (int i = 0; i < 2; ++i)
        s.blocked_requests.fetch_add(1, std::memory_order_relaxed);
    for (int i = 0; i < 4; ++i)
        s.cache_hits.fetch_add(1, std::memory_order_relaxed);
    for (int i = 0; i < 1; ++i)
        s.cache_misses.fetch_add(1, std::memory_order_relaxed);

    EXPECT(s.total_requests  .load() == 5);
    EXPECT(s.allowed_requests.load() == 3);
    EXPECT(s.blocked_requests.load() == 2);
    EXPECT(s.cache_hits      .load() == 4);
    EXPECT(s.cache_misses    .load() == 1);
}

// snapshot() should return a plain struct matching the live counter values.
void test_snapshot()
{
    aegis::AtomicStats s;
    s.total_requests  .fetch_add(10, std::memory_order_relaxed);
    s.allowed_requests.fetch_add(7,  std::memory_order_relaxed);
    s.blocked_requests.fetch_add(3,  std::memory_order_relaxed);
    s.cache_hits      .fetch_add(5,  std::memory_order_relaxed);
    s.cache_misses    .fetch_add(2,  std::memory_order_relaxed);

    auto snap = s.snapshot();
    EXPECT(snap.total_requests   == 10);
    EXPECT(snap.allowed_requests == 7);
    EXPECT(snap.blocked_requests == 3);
    EXPECT(snap.cache_hits       == 5);
    EXPECT(snap.cache_misses     == 2);
}

// print() should emit output containing the counter values.
void test_print_output()
{
    aegis::AtomicStats s;
    s.total_requests.fetch_add(42, std::memory_order_relaxed);

    std::ostringstream oss;
    s.print(oss);
    std::string out = oss.str();

    EXPECT(out.find("total=42")  != std::string::npos);
    EXPECT(out.find("[stats]")   != std::string::npos);
}

// ── Concurrent increment test ─────────────────────────────────────────────────
//
// This is the most important test.  N_THREADS threads each increment
// total_requests N_OPS times.  The expected final value is N_THREADS * N_OPS.
//
// If the counters were plain (non-atomic) integers, the lost-update race
// would produce a final value less than N_THREADS * N_OPS — the test would
// fail.  With std::atomic the hardware ensures every increment is serialised
// at the cache-line level, so the total is always exact.
void test_concurrent_increments()
{
    constexpr int N_THREADS = 8;
    constexpr int N_OPS     = 10'000;

    aegis::AtomicStats s;

    auto worker = [&]() {
        for (int i = 0; i < N_OPS; ++i) {
            s.total_requests  .fetch_add(1, std::memory_order_relaxed);
            s.allowed_requests.fetch_add(1, std::memory_order_relaxed);
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(N_THREADS);
    for (int i = 0; i < N_THREADS; ++i)
        threads.emplace_back(worker);
    for (auto& t : threads) t.join();

    const std::uint64_t expected = static_cast<std::uint64_t>(N_THREADS) * N_OPS;
    EXPECT(s.total_requests  .load() == expected);
    EXPECT(s.allowed_requests.load() == expected);
    EXPECT(s.blocked_requests.load() == 0);   // not touched
    EXPECT(s.cache_hits      .load() == 0);   // not touched
    EXPECT(s.cache_misses    .load() == 0);   // not touched
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main()
{
    std::cout << "=== AtomicStats tests ===\n\n";

    run_test("initial_zero",            test_initial_zero);
    run_test("single_thread_increment", test_single_thread_increment);
    run_test("snapshot",                test_snapshot);
    run_test("print_output",            test_print_output);
    run_test("concurrent_increments",   test_concurrent_increments);

    std::cout << "\n=== Results: "
              << g_passed << " passed, "
              << g_failed << " failed ===\n";

    return g_failed == 0 ? 0 : 1;
}
