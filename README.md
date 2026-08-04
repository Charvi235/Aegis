# Aegis — High-Throughput API Gateway & Rate Limiter

A portfolio/learning project demonstrating systems programming in C++17:
async I/O with Boost.Asio/Beast, a hand-rolled thread pool, a token-bucket
rate limiter, an LRU response cache, and a reverse proxy.

## Tech stack

- **C++17** — `std::thread`, `std::mutex`, `std::chrono`, structured bindings
- **Boost.Asio** — async TCP acceptor, strand-based handler dispatch
- **Boost.Beast** — HTTP/1.1 request parsing and response serialisation
- **CMake 3.16+** — single build system for all platforms

## Build

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

## Run

```sh
./build/aegis [port] [threads]
# defaults: port=8080, threads=hardware_concurrency
```

## Stages

| Stage | Feature |
|-------|---------|
| 1 | Async TCP server — accept & log connections |
| 2 | Boost.Beast HTTP — static "hello" response |
| 3 | Token bucket rate limiter (isolated, no locking) |
| 4 | Per-IP buckets wired into request path with mutex |
| 5 | Thread-safe LRU response cache |
| 6 | Reverse proxy — full end-to-end flow |
