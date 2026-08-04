// ─────────────────────────────────────────────────────────────────────────────
// test/test_lru_cache.cpp
//
// Unit tests for LruCache<string,string>.
// Same hand-rolled harness as test_token_bucket.cpp.
//
// Run:
//   cmake --build build && ./build/test_lru_cache
// ─────────────────────────────────────────────────────────────────────────────

#include "aegis/lru_cache.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// ── Minimal harness ───────────────────────────────────────────────────────────

static int g_passed = 0;
static int g_failed = 0;

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

static void run_test(const std::string& name, void(*fn)())
{
    std::cout << "[ TEST ] " << name << "\n";
    fn();
}

// ── Tests ─────────────────────────────────────────────────────────────────────

// A cache miss returns nullopt.
void test_miss()
{
    aegis::LruCache<std::string, std::string> c{3};
    EXPECT(!c.get("x").has_value());
}

// A put followed by a get returns the value.
void test_basic_put_get()
{
    aegis::LruCache<std::string, std::string> c{3};
    c.put("a", "alpha");
    auto v = c.get("a");
    EXPECT(v.has_value());
    EXPECT(v.value() == "alpha");
}

// Putting the same key twice updates the value.
void test_update()
{
    aegis::LruCache<std::string, std::string> c{3};
    c.put("k", "v1");
    c.put("k", "v2");
    EXPECT(c.get("k").value() == "v2");
    EXPECT(c.size() == 1);
}

// When capacity is exceeded, the least-recently-used item is evicted.
// Order: put a, put b, put c  →  capacity is full.
// put d  →  a (LRU) should be evicted.
void test_eviction_lru()
{
    aegis::LruCache<std::string, std::string> c{3};
    c.put("a", "1");
    c.put("b", "2");
    c.put("c", "3");
    c.put("d", "4");   // should evict "a"

    EXPECT(!c.get("a").has_value());   // evicted
    EXPECT(c.get("b").has_value());
    EXPECT(c.get("c").has_value());
    EXPECT(c.get("d").has_value());
    EXPECT(c.size() == 3);
}

// Accessing an item promotes it, so it is no longer LRU.
// put a, put b, put c → access a → put d → b should be evicted (not a).
void test_access_promotes()
{
    aegis::LruCache<std::string, std::string> c{3};
    c.put("a", "1");
    c.put("b", "2");
    c.put("c", "3");
    c.get("a");        // promote a to MRU
    c.put("d", "4");   // should evict b (now LRU)

    EXPECT(c.get("a").has_value());    // promoted — survives
    EXPECT(!c.get("b").has_value());   // evicted
    EXPECT(c.get("c").has_value());
    EXPECT(c.get("d").has_value());
}

// Explicit eviction removes the key.
void test_explicit_evict()
{
    aegis::LruCache<std::string, std::string> c{3};
    c.put("x", "val");
    c.evict("x");
    EXPECT(!c.get("x").has_value());
    EXPECT(c.size() == 0);
}

// Evicting a non-existent key is a no-op (doesn't throw or corrupt state).
void test_evict_missing()
{
    aegis::LruCache<std::string, std::string> c{3};
    c.put("a", "1");
    c.evict("z");      // "z" never inserted
    EXPECT(c.size() == 1);
    EXPECT(c.get("a").has_value());
}

// Zero capacity should throw at construction.
void test_zero_capacity_throws()
{
    bool threw = false;
    try {
        aegis::LruCache<std::string, std::string> c{0};
        (void)c;
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    EXPECT(threw);
}

// Capacity-1 cache: every new insertion evicts the previous entry.
void test_capacity_one()
{
    aegis::LruCache<std::string, std::string> c{1};
    c.put("a", "1");
    c.put("b", "2");
    EXPECT(!c.get("a").has_value());
    EXPECT(c.get("b").has_value());
}

// Thread safety: concurrent puts and gets must not corrupt the cache.
// We don't check exact final state (race on which key survives is fine),
// just that no crash or UB occurs.
void test_concurrent_access()
{
    aegis::LruCache<std::string, std::string> c{50};

    constexpr int NUM_THREADS = 8;
    constexpr int OPS_PER_THREAD = 500;

    auto worker = [&](int id) {
        for (int i = 0; i < OPS_PER_THREAD; ++i) {
            std::string key = "k" + std::to_string(i % 20);
            std::string val = "t" + std::to_string(id) + "_" + std::to_string(i);
            c.put(key, val);
            c.get(key);
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(NUM_THREADS);
    for (int i = 0; i < NUM_THREADS; ++i)
        threads.emplace_back(worker, i);
    for (auto& t : threads) t.join();

    // If we get here without a crash / assertion failure, thread safety holds.
    EXPECT(c.size() <= 50);
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main()
{
    std::cout << "=== LruCache tests ===\n\n";

    run_test("miss",                  test_miss);
    run_test("basic_put_get",         test_basic_put_get);
    run_test("update",                test_update);
    run_test("eviction_lru",          test_eviction_lru);
    run_test("access_promotes",       test_access_promotes);
    run_test("explicit_evict",        test_explicit_evict);
    run_test("evict_missing",         test_evict_missing);
    run_test("zero_capacity_throws",  test_zero_capacity_throws);
    run_test("capacity_one",          test_capacity_one);
    run_test("concurrent_access",     test_concurrent_access);

    std::cout << "\n=== Results: "
              << g_passed << " passed, "
              << g_failed << " failed ===\n";

    return g_failed == 0 ? 0 : 1;
}
