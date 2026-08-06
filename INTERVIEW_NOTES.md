# Interview Notes — Aegis API Gateway

This file maps each resume bullet point to the exact code that implements it,
with a plain-English explanation of how it works and why key design decisions
were made.

---

## Bullet 1

> "Engineered a high-throughput API Gateway with a custom Token Bucket
> rate-limiter to throttle malicious traffic via HTTP 429 responses"

### Where in the code

| What | Location |
|------|----------|
| Token bucket algorithm | `include/aegis/token_bucket.hpp`, `src/token_bucket.cpp` |
| Per-IP dispatch | `include/aegis/rate_limiter.hpp`, `src/rate_limiter.cpp` |
| HTTP 429 enforcement | `src/session.cpp` → `Session::on_read()` and `Session::make_rate_limit_response()` |
| Unit tests | `test/test_token_bucket.cpp` (21 tests) |

### How it works

A token bucket is like a leaky bucket running in reverse: it fills with tokens
at a fixed rate (`refill_rate` tokens per second) up to a maximum burst size
(`capacity`). Each incoming request costs one token. If the bucket is empty the
request is rejected and Aegis immediately returns an `HTTP 429 Too Many Requests`
response with a `Retry-After` header that tells the client how long to wait.

The critical design choice is **lazy refill**: instead of running a background
timer that tops up every bucket every N milliseconds, `TokenBucket::consume()`
calls `refill()` first, which reads `std::chrono::steady_clock::now()`, computes
how many tokens the elapsed time has earned, adds them (capped at capacity), and
only then checks whether a token is available. This costs one clock read per
request and zero background threads — the same strategy used by nginx, Envoy,
and AWS API Gateway. The alternative (a timer thread per bucket) would be
catastrophically expensive at millions of clients.

`steady_clock` is used (not `system_clock`) because it is monotonic — it never
goes backwards due to NTP slews or daylight-saving changes. A backwards jump in
a wall clock would make `elapsed` negative and add an enormous phantom token
windfall, allowing a burst that bypasses the limit.

The `Retry-After` value is computed by `seconds_until_token()`:
`(1.0 - tokens_) / refill_rate_` — the exact fractional seconds until the next
token arrives, ceiling-rounded to the nearest whole second for the header.

---

## Bullet 2

> "Implemented a thread-safe LRU Cache combining Hash Maps and Doubly Linked
> Lists for O(1) query retrieval and eviction"

### Where in the code

| What | Location |
|------|----------|
| Full implementation (header-only template) | `include/aegis/lru_cache.hpp` |
| Cache integration in session | `src/session.cpp` → `Session::on_read()` and `Session::on_proxy_response()` |
| Cache invalidation on mutation | `src/session.cpp` → `Session::on_read()`, non-GET branch |
| Unit tests | `test/test_lru_cache.cpp` (22 tests, including 8-thread concurrency test) |

### How it works

The LRU cache keeps two data structures in sync:

- **`std::list<pair<Key,Value>>`** — a doubly linked list ordered from
  most-recently-used (front) to least-recently-used (back).
- **`std::unordered_map<Key, list::iterator>`** — maps each key directly to its
  node in the list.

`get(key)` is O(1): look up the iterator in the hash map in O(1), then call
`list::splice()` to move that node to the front in O(1). Splice just
rewires three pointers — no allocation, no copy.

`put(key, value)` is O(1): if the key exists, update and splice to front. If
new, `emplace_front` at the head and insert the iterator into the map. If the
list now exceeds capacity, read `list_.back().first` (the LRU key) to erase it
from the map, then `pop_back()` from the list — both O(1).

The reason this beats a plain `std::map` (which gives O(log N) access) is that
`std::list` iterators are **stable**: insertions and splices never invalidate
any other iterator. Storing a list iterator in the hash map and splicing it to
the front on each access is the classic O(1) LRU trick — you get the hash map's
O(1) lookup and the list's O(1) pointer rewiring in the same operation.

Thread safety is provided by a single `std::mutex` that wraps every public
method. The critical section is a hash lookup plus three pointer assignments —
nanoseconds of work — so contention against network I/O latency (milliseconds)
is negligible. The mutex is declared `mutable` so `size()` and `empty()` can
lock from a `const` context.

---

## Bullet 3

> "Synchronized concurrent worker threads using Mutex locks to eliminate race
> conditions during high-speed token deductions"

### Where in the code

| What | Location |
|------|----------|
| Mutex-protected token map | `include/aegis/rate_limiter.hpp` → `std::mutex mutex_` |
| Lock scope (lookup + consume atomic) | `src/rate_limiter.cpp` → `RateLimiter::is_allowed()` |
| Why single lock (design comment) | `include/aegis/rate_limiter.hpp`, "Lock granularity choice" section |
| Thread pool that drives concurrency | `src/server.cpp` → `Server::run()` |
| LRU cache mutex | `include/aegis/lru_cache.hpp` → `mutable std::mutex mutex_` |

### How it works

`Server` spawns `num_threads` worker threads, each calling `io_ctx_.run()`.
Asio dispatches accepted connections across these threads, so multiple
`Session::on_read()` handlers can execute simultaneously for different clients.

The race condition without locking: suppose two threads both reach
`RateLimiter::is_allowed("1.2.3.4")` at the same moment. Thread A reads
`tokens_ = 1.0` and decides to allow. Before A subtracts the token, Thread B
also reads `tokens_ = 1.0` and also decides to allow. Both threads return `true`
— one token was consumed twice, and the bucket is now at -1.0 instead of 0.0.
This kind of check-then-act race is a classic TOCTOU (Time-Of-Check /
Time-Of-Use) bug.

The fix is `std::lock_guard<std::mutex>` that spans **both** the map lookup
and the `consume()` call inside `is_allowed()`. The lock makes the
check-and-deduct atomic from the perspective of all other threads. No thread
can read `tokens_` between another thread's read and subtract.

Why not a per-bucket lock (one mutex per IP)? That would allow truly parallel
updates for different IPs, but it requires either storing a mutex inside
`TokenBucket` (mixing threading concern into the algorithm class) or a parallel
container of mutexes. A single map-level mutex is simpler, correct, and fast
enough because the critical section is just a hash map lookup (O(1), ~10 ns)
plus a few floating-point operations — negligible compared to the TCP I/O
overhead that surrounds it. The design comment in `rate_limiter.hpp` explains
the sharded-mutex upgrade path if higher concurrency is ever needed.

---

## Quick reference: key files

```
include/aegis/
  token_bucket.hpp   ← TokenBucket algorithm + design docs
  rate_limiter.hpp   ← mutex strategy, lock granularity rationale
  lru_cache.hpp      ← full O(1) LRU implementation (template, header-only)
  session.hpp        ← per-connection HTTP pipeline
  proxy.hpp          ← async backend proxy pipeline
  server.hpp         ← io_context, thread pool, shared resources

src/
  token_bucket.cpp   ← consume(), refill(), seconds_until_token()
  rate_limiter.cpp   ← is_allowed(), retry_after(), get_or_create()
  session.cpp        ← on_read (rate limit + cache check), on_proxy_response
  proxy.cpp          ← resolve → connect → write → read pipeline
  server.cpp         ← run(), do_accept(), stop()
  main.cpp           ← argument parsing, signal handling

test/
  test_token_bucket.cpp  ← 21 tests: drain, refill timing, cap, retry-after
  test_lru_cache.cpp     ← 22 tests: eviction order, promotion, concurrency
```
