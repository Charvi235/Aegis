// ─────────────────────────────────────────────────────────────────────────────
// src/token_bucket.cpp
// ─────────────────────────────────────────────────────────────────────────────

#include "aegis/token_bucket.hpp"

#include <algorithm>   // std::min

namespace aegis {

// ── Constructor ──────────────────────────────────────────────────────────────

TokenBucket::TokenBucket(double capacity, double refill_rate)
    : capacity_{capacity}
    , refill_rate_{refill_rate}
    , tokens_{capacity}          // start full — new clients get their burst
    , last_refill_{Clock::now()}
{}

// ── Public interface ─────────────────────────────────────────────────────────

bool TokenBucket::consume()
{
    // Always refill first so we credit any earned tokens before deciding.
    refill();

    if (tokens_ >= 1.0) {
        tokens_ -= 1.0;
        return true;   // allowed
    }

    return false;      // bucket empty — rate limit exceeded
}

double TokenBucket::seconds_until_token() const
{
    if (tokens_ >= 1.0) return 0.0;

    // We need (1.0 - tokens_) more tokens.
    // At refill_rate_ tokens/second that takes:
    double deficit = 1.0 - tokens_;
    return deficit / refill_rate_;
}

// ── Private helpers ───────────────────────────────────────────────────────────

void TokenBucket::refill()
{
    TimePoint now     = Clock::now();
    // std::chrono::duration<double> converts the tick count to fractional
    // seconds automatically, regardless of the clock's native resolution.
    double elapsed    = std::chrono::duration<double>(now - last_refill_).count();
    double new_tokens = elapsed * refill_rate_;

    // Add earned tokens, but never exceed capacity.
    // std::min keeps the arithmetic simple and avoids a separate if-branch.
    tokens_ = std::min(capacity_, tokens_ + new_tokens);

    // Advance the refill timestamp regardless of whether we actually added
    // any tokens.  If we capped at capacity, the surplus time is discarded —
    // we don't "bank" time beyond the bucket size.  This is intentional:
    // allowing banked time would let clients accumulate debt against future
    // bursts, defeating the capacity limit.
    last_refill_ = now;
}

} // namespace aegis
