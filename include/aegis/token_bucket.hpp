#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// aegis/token_bucket.hpp
//
// A single token-bucket rate limiter for one client.
//
// ── How a token bucket works ─────────────────────────────────────────────────
// Imagine a bucket that holds up to `capacity` tokens.  Tokens drip in at
// `refill_rate` tokens/second.  Each allowed request costs one token.
// If the bucket is empty the request is rejected (HTTP 429).
//
// Compared to a fixed window counter ("max N requests per minute") a token
// bucket is burst-friendly: a client can spend a burst of saved-up tokens
// quickly, then is throttled once they run out.  Fixed windows punish bursty
// but otherwise legitimate traffic; token buckets don't.
//
// ── Lazy refill vs background timer ──────────────────────────────────────────
// Two common implementation strategies:
//
//   A) Background timer thread: a thread runs every T ms and adds tokens to
//      every bucket.  Simple to reason about, but requires a thread (or at
//      least a timer async op) per bucket or per rate — expensive when you
//      have millions of clients.
//
//   B) Lazy refill (chosen here): no background thread.  Instead, when a
//      request arrives, we compute how much wall-clock time has elapsed since
//      the last refill, calculate how many tokens that duration earns, add
//      them (capped at capacity), and *then* decide whether to allow the
//      request.  Cost: one clock read per request.  Zero background threads.
//
// Lazy refill is the standard choice for high-throughput gateways (nginx
// rate_limit, Envoy, AWS API Gateway all use variants of it).
//
// ── Thread safety ────────────────────────────────────────────────────────────
// This class is NOT thread-safe on its own.  Locking is added in Stage 4
// at the RateLimiter layer (which owns a map of TokenBuckets).  Keeping the
// two concerns separate makes each class simpler and easier to test.
//
// ── Clock choice ─────────────────────────────────────────────────────────────
// std::chrono::steady_clock is monotonic — it never goes backwards, even
// when the system clock is adjusted (NTP, DST, etc.).  That matters here:
// a backwards jump in a wall clock could make elapsed_seconds negative,
// adding a huge token windfall.  steady_clock prevents that.
// ─────────────────────────────────────────────────────────────────────────────

#include <chrono>

namespace aegis {

class TokenBucket {
public:
    using Clock    = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    // capacity     – maximum tokens the bucket can hold (also the burst limit)
    // refill_rate  – tokens added per second (may be fractional, e.g. 0.5)
    //
    // Example: capacity=10, refill_rate=2 means a client can burst 10 requests
    // immediately, then sustains 2 req/s thereafter.
    TokenBucket(double capacity, double refill_rate);

    // Attempt to consume one token.
    // Returns true  if the request is allowed (a token was consumed).
    // Returns false if the bucket is empty (caller should return HTTP 429).
    //
    // Side effect: always performs a lazy refill before checking.
    bool consume();

    // How many whole seconds the client must wait before the next token
    // arrives.  Useful for the Retry-After response header.
    // Returns 0 if tokens are already available.
    double seconds_until_token() const;

    // ── Accessors for testing ─────────────────────────────────────────────
    double tokens()   const { return tokens_; }
    double capacity() const { return capacity_; }

private:
    // Add tokens earned since last_refill_, capped at capacity_.
    // Updates last_refill_ to now.
    void refill();

    double    capacity_;      // bucket size
    double    refill_rate_;   // tokens per second
    double    tokens_;        // current token count (fractional is fine)
    TimePoint last_refill_;   // when we last added tokens
};

} // namespace aegis
