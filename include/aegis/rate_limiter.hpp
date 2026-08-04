#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// aegis/rate_limiter.hpp
//
// RateLimiter: a thread-safe map from client IP → TokenBucket.
//
// ── Responsibility split ──────────────────────────────────────────────────────
// TokenBucket  (Stage 3) knows nothing about concurrency.  It is a pure
// algorithm: given a clock reading, decide allow/deny and update state.
//
// RateLimiter  (this class) adds exactly one concern: safe concurrent access
// to a *collection* of buckets from multiple worker threads.
//
// Keeping the two separate means:
//   - TokenBucket is trivially testable without threads.
//   - RateLimiter's locking strategy can be changed (e.g. per-bucket locks,
//     sharded map) without touching the core algorithm.
//
// ── Lock granularity choice ───────────────────────────────────────────────────
// We use a single std::mutex for the whole map.
//
// Why not per-bucket locks?
//   Per-bucket locking allows truly parallel updates for different IPs.
//   But it requires either (a) a mutex stored inside each bucket, making
//   TokenBucket aware of threading, or (b) a separate parallel container
//   of mutexes.  Both add complexity.  For a portfolio project where the
//   bottleneck is network I/O, a single map mutex is simpler and correct.
//   The lock is held only for the duration of the bucket lookup + consume
//   call — a few nanoseconds — so contention is minimal in practice.
//
//   If you wanted per-bucket locking later, the natural approach is a
//   "striped" lock (a fixed array of N mutexes, bucket selected by
//   hash(ip) % N), which amortises lock contention without per-entry overhead.
//
// ── Why shared_ptr for RateLimiter? ──────────────────────────────────────────
// Server owns the RateLimiter and passes a shared_ptr into every Session.
// This avoids raw pointer aliasing (dangerous if Server is destroyed while
// sessions are still live) and makes the ownership chain explicit.
// ─────────────────────────────────────────────────────────────────────────────

#include "aegis/token_bucket.hpp"

#include <mutex>
#include <string>
#include <unordered_map>

namespace aegis {

class RateLimiter {
public:
    // capacity    – max tokens per bucket (burst limit per IP)
    // refill_rate – tokens/second added per bucket
    RateLimiter(double capacity, double refill_rate);

    // Check whether the given client IP is allowed to proceed.
    //
    // If allowed: consumes one token and returns true.
    // If denied:  leaves the bucket untouched and returns false.
    //             Call retry_after(ip) to get the Retry-After seconds value.
    //
    // Thread-safe: multiple worker threads may call this concurrently for
    // different or the same IP.
    bool is_allowed(const std::string& ip);

    // Seconds until the next token is available for `ip`.
    // Returns 0 if the IP has tokens (i.e. is_allowed would return true).
    // Also thread-safe.
    double retry_after(const std::string& ip);

private:
    // Find-or-create a bucket for `ip`.  Caller must hold mutex_.
    TokenBucket& get_or_create(const std::string& ip);

    double capacity_;
    double refill_rate_;

    std::mutex                                   mutex_;
    std::unordered_map<std::string, TokenBucket> buckets_;
};

} // namespace aegis
