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
| **Dependencies** | Boost >=1.74 (header-only Asio + Beast; `system` + `thread` libs) |

---

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

**Class responsibilities:**

| Class | Responsibility |
|-------|---------------|
| `TokenBucket` | Single-client token-bucket rate limiter; lazy-refill algorithm using `steady_clock`, no background threads. |
| `RateLimiter` | Thread-safe map of IP → TokenBucket; single `std::mutex` guards the map across all worker threads. |
| `LruCache<K,V>` | Fixed-capacity LRU cache using a doubly-linked list + hash map for O(1) get, put, and eviction. |
| `Session` | Owns one client TCP socket; reads HTTP request, enforces rate limit, checks cache, and drives the proxy pipeline. |
| `Proxy` | Owns one backend TCP socket per request; async DNS resolve → connect → write → read pipeline; delivers response via callback. |
| `Server` | Creates the `io_context`, thread pool, `tcp::acceptor`, and shared `RateLimiter`/`LruCache`; spawns a `Session` per accepted connection. |

**Request flow:**
1. Client connects → `Server` accepts → creates `Session`
2. `Session` reads full HTTP request
3. `RateLimiter` checks token bucket for client IP → 429 if empty
4. `LruCache` checks for cached response → return immediately on HIT
5. `Proxy` resolves backend DNS, connects, forwards request, reads response
6. Cache stores 200 GET responses; mutating methods evict stale entries
7. `Session` writes response back to client; honours HTTP keep-alive

---

## Build

### Prerequisites

- CMake 3.16+
- Boost 1.74+ (Asio, Beast, system, thread)
- A C++17 compiler

**Windows (MSVC via Visual Studio Build Tools):**

```powershell
# Open a Developer PowerShell / x64 Native Tools Command Prompt, then:
cmake -B build -S .
cmake --build build
# Binaries land in build\Debug\
```

If cmake can't find Boost automatically, point it at your installation:

```powershell
cmake -B build -S . -DBOOST_ROOT="C:\path\to\boost"
cmake --build build
```

**Linux / macOS:**

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build
# Binary: build/aegis
```

---

## Run

```
aegis.exe [port] [threads] [rl_capacity] [rl_refill_rate] [cache_capacity] [backend_host] [backend_port]
```

| Argument | Default | Description |
|----------|---------|-------------|
| `port` | `8080` | Port Aegis listens on |
| `threads` | `hw_concurrency` | Worker thread count |
| `rl_capacity` | `10` | Token bucket burst size per IP |
| `rl_refill_rate` | `5` | Tokens refilled per second per IP |
| `cache_capacity` | `256` | Max cached GET responses |
| `backend_host` | `localhost` | Upstream backend hostname |
| `backend_port` | `9090` | Upstream backend port |

**Quick start — demo with Python mock backend:**

```powershell
# Terminal 1: start mock backend (serves local files over HTTP)
python -m http.server 9090

# Terminal 2: start Aegis
# Using tight rate limits (5 tokens, 1 tok/s) so 429 is easy to trigger
.\build\Debug\aegis.exe 8080 2 5 1 256 localhost 9090
```

---

## Test

```powershell
# Run unit tests via CTest
ctest --test-dir build --output-on-failure

# Or run the test executables directly
.\build\Debug\test_token_bucket.exe
.\build\Debug\test_lru_cache.exe
```

Expected output:
```
=== TokenBucket tests ===
[ TEST ] starts_full
[ TEST ] consume_decrements
...
=== Results: 21 passed, 0 failed ===

=== LruCache tests ===
[ TEST ] miss
[ TEST ] basic_put_get
...
=== Results: 22 passed, 0 failed ===
```

---

## Demo: Cache HIT/MISS and Rate Limiting (429)

With Aegis running on port 8080 and a backend on port 9090:

### 1. First GET — cache MISS

```bash
curl -i http://localhost:8080/README.md
```

Expected response:
```
HTTP/1.1 200 OK
Server: Aegis/0.1
Content-Type: text/html; charset=utf-8
X-Cache: MISS
Content-Length: 4252

# Aegis — High-Throughput API Gateway...
```

### 2. Same GET again — cache HIT

```bash
curl -i http://localhost:8080/README.md
```

Expected response:
```
HTTP/1.1 200 OK
Server: Aegis/0.1
Content-Type: text/plain
X-Cache: HIT
Content-Length: 4252

# Aegis — High-Throughput API Gateway...
```

The second response is served from the LRU cache — the backend is never contacted.

### 3. Burst beyond rate limit — HTTP 429

Send 10 rapid requests. With default settings (capacity=5), requests 6–10 will be rejected:

```bash
# Linux/macOS — fire 10 requests as fast as possible
for i in $(seq 1 10); do
  echo -n "Request $i: "
  curl -s -o /dev/null -w "%{http_code} (Retry-After: %header{retry-after})\n" \
    http://localhost:8080/README.md
done
```

```powershell
# Windows PowerShell
1..10 | ForEach-Object {
    $r = Invoke-WebRequest -Uri "http://localhost:8080/README.md" `
         -SkipHttpErrorCheck -UseBasicParsing
    "Request $_ : HTTP $($r.StatusCode)   X-Cache: $($r.Headers['X-Cache'])   Retry-After: $($r.Headers['Retry-After'])"
}
```

Expected output (with capacity=5, refill=1 tok/s):
```
Request 1 : HTTP 200   X-Cache: HIT
Request 2 : HTTP 200   X-Cache: HIT
Request 3 : HTTP 200   X-Cache: HIT
Request 4 : HTTP 200   X-Cache: HIT
Request 5 : HTTP 200   X-Cache: HIT
Request 6 : HTTP 429   X-Cache:    Retry-After: 1
Request 7 : HTTP 429   X-Cache:    Retry-After: 1
Request 8 : HTTP 429   X-Cache:    Retry-After: 1
Request 9 : HTTP 429   X-Cache:    Retry-After: 1
Request 10: HTTP 429   X-Cache:    Retry-After: 1
```

The 429 body also contains: `Rate limit exceeded. Retry after 1 second(s).`

### 4. Cache invalidation via POST

A POST to the same path evicts the cached GET response, so the next GET triggers a fresh MISS:

```bash
curl -i -X POST http://localhost:8080/README.md   # evicts cache entry
curl -i        http://localhost:8080/README.md    # X-Cache: MISS again
```

---

## Stages

| Stage | Feature | Files |
|-------|---------|-------|
| 1 | Async TCP server, thread pool | `server.hpp/cpp` |
| 2 | Beast HTTP, static response | `session.hpp/cpp` |
| 3 | Token bucket (isolated) | `token_bucket.hpp/cpp`, `test_token_bucket.cpp` |
| 4 | Per-IP rate limiting with mutex | `rate_limiter.hpp/cpp` |
| 5 | Thread-safe LRU cache | `lru_cache.hpp`, `test_lru_cache.cpp` |
| 6 | Reverse proxy end-to-end | `proxy.hpp/cpp` |

---

## Design notes

**Lazy refill** — `TokenBucket` computes elapsed time on each request rather than
using a background timer thread. Zero extra threads, one `steady_clock` read per
request. This is the same strategy used by nginx, Envoy, and AWS API Gateway.

**Single io_context, N threads** — simpler than per-thread `io_context`s; Asio
handles load-balancing automatically.

**Single mutex per collection** — both `RateLimiter` and `LruCache` use one
`std::mutex`. The critical section (hash lookup + pointer update) is
nanoseconds wide; contention is negligible vs network I/O latency. A sharded
mutex (hash(ip) % N buckets) would be the next step for very high concurrency.

**Buffered proxy** — the full backend response is read before forwarding.
Streaming (read-chunk → write-chunk) would reduce latency for large bodies
but requires two concurrent async loops and backpressure management.

**Body-only caching** — only the response body is cached, not the full
serialised message. This avoids forwarding hop-by-hop headers (`Connection`,
`Transfer-Encoding`) and stale `Date` headers from the backend.

**Explicit socket close on all error paths** — `Session::do_close()` calls
`shutdown_both` then `close()`, releasing the OS file descriptor immediately
rather than waiting for the `shared_ptr` destructor. `proxy.cpp` mirrors this
on connect and write failures.
