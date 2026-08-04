// ─────────────────────────────────────────────────────────────────────────────
// src/rate_limiter.cpp
// ─────────────────────────────────────────────────────────────────────────────

#include "aegis/rate_limiter.hpp"

namespace aegis {

// ── Constructor ──────────────────────────────────────────────────────────────

RateLimiter::RateLimiter(double capacity, double refill_rate)
    : capacity_{capacity}
    , refill_rate_{refill_rate}
{}

// ── Public interface ─────────────────────────────────────────────────────────

bool RateLimiter::is_allowed(const std::string& ip)
{
    // Lock for the duration of the lookup AND the consume call.
    //
    // Why lock across both operations?
    //   If we released the lock between find() and consume(), another thread
    //   could interleave and consume the last token, leaving this thread with
    //   a stale "tokens available" answer.  Keeping the lock through the
    //   consume() makes the check-then-act atomic.
    std::lock_guard<std::mutex> lock{mutex_};
    return get_or_create(ip).consume();
}

double RateLimiter::retry_after(const std::string& ip)
{
    std::lock_guard<std::mutex> lock{mutex_};
    return get_or_create(ip).seconds_until_token();
}

// ── Private helpers ───────────────────────────────────────────────────────────

TokenBucket& RateLimiter::get_or_create(const std::string& ip)
{
    // try_emplace is the right tool here:
    //   - If the key exists, it returns the existing element without
    //     constructing a new TokenBucket (no wasted constructor call).
    //   - If the key is absent, it constructs a bucket in-place with
    //     (capacity_, refill_rate_), inserting it atomically.
    //
    // The alternative — operator[] followed by a manual initialisation check
    // — would default-construct a bucket first (with zeros), then overwrite,
    // which is wasteful and error-prone.
    auto [it, _inserted] = buckets_.try_emplace(ip, capacity_, refill_rate_);
    return it->second;
}

} // namespace aegis
