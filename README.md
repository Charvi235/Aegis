# Aegis — High-Throughput API Gateway & Rate Limiter

A portfolio/learning project demonstrating systems programming in C++17:
async I/O with Boost.Asio/Beast, a hand-rolled thread pool, a token-bucket
rate limiter, an LRU response cache, and a full reverse proxy.

## Tech stack

| | |
|---|---|
| **Language** | C++17 |
| **Async I/O** | Boost.Asio (shared `io_context`, N-thread pool) |
| **HTTP** | Boost.Beast (HTTP/1.1 parse + serialise) |
| **Build** | CMake 3.16+ |
| **Concurrency** | `std::thread`, `std::mutex` — no external thread pool library |

## Architecture

```
Client
  │  HTTP/1.1
  ▼
┌─────────────────────────────────────────────┐
│  Server  (tcp::acceptor, thread pool)        │
│    └─ Session  (per-connection)              │
│         │                                    │
│         ├─ RateLimiter                       │
│         │    └─ TokenBucket  (per client IP) │
│         │                                    │
│         ├─ LruCache  (GET response cache)    │
│         │                                    │
│         └─ Proxy  (per request, async)       │
└───────────────────┬─────────────────────────┘
                    │  HTTP/1.1
                    ▼
               Backend server
```

**Request flow:**
1. Client connects → `Server` accepts → creates `Session`
2. `Session` reads full HTTP request
3. `RateLimiter` checks token bucket for client IP → 429 if empty
4. `LruCache` checks for cached response → return immediately on HIT
5. `Proxy` resolves backend DNS, connects, forwards request, reads response
6. Cache stores 200 GET responses; mutating methods evict stale entries
7. `Session` writes response back to client; honours HTTP keep-alive

## Build

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

## Run

```sh
# Start a mock backend (Python standard library, no install needed)
python -m http.server 9090

# Start Aegis
./build/aegis [port] [threads] [rl_capacity] [rl_refill_rate] [cache_capacity] [backend_host] [backend_port]

# Defaults: port=8080, threads=hw_concurrency, rl=10tok/5tok/s, cache=256, backend=localhost:9090
./build/aegis

# Custom example: 4 threads, tight rate limit, smaller cache, different backend
./build/aegis 8080 4 5 2 64 api.example.com 80
```

## Test

```sh
cmake --build build
ctest --test-dir build --output-on-failure
```

## Stages

| Stage | Feature | Files |
|-------|---------|-------|
| 1 | Async TCP server, thread pool | `server.hpp/cpp` |
| 2 | Beast HTTP, static response | `session.hpp/cpp` |
| 3 | Token bucket (isolated) | `token_bucket.hpp/cpp`, `test_token_bucket.cpp` |
| 4 | Per-IP rate limiting with mutex | `rate_limiter.hpp/cpp` |
| 5 | Thread-safe LRU cache | `lru_cache.hpp`, `test_lru_cache.cpp` |
| 6 | Reverse proxy end-to-end | `proxy.hpp/cpp` |

## Design notes

**Lazy refill** — TokenBucket computes elapsed time on each request rather than
using a background timer thread. Zero extra threads, one clock read per request.

**Single io_context, N threads** — simpler than per-thread io_contexts; Asio
handles load-balancing automatically. Per-thread contexts give better cache
locality but require work-stealing.

**Single mutex per collection** — both `RateLimiter` and `LruCache` use one
`std::mutex`. The critical section (hash lookup + pointer update) is
nanoseconds wide; contention is negligible vs network I/O latency. A sharded
mutex would be the next step for higher concurrency.

**Buffered proxy** — the full backend response is read before forwarding.
Streaming (read-chunk → write-chunk) would reduce latency for large bodies
but requires two concurrent async loops and backpressure management.

**Body-only caching** — only the response body is cached, not the full
serialised message. This avoids forwarding hop-by-hop headers (Connection,
Transfer-Encoding) and stale Date headers from the backend.
