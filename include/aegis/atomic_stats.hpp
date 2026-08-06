#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// aegis/atomic_stats.hpp
//
// Stage 7: lock-free gateway metrics using std::atomic counters.
//
// ── Why std::atomic and not a mutex? ─────────────────────────────────────────
// Each counter here is a single 64-bit integer that is only ever incremented
// by one.  The entire operation is a single read-modify-write — it maps
// directly to one hardware instruction: LOCK XADD on x86, LDADD on ARM.
//
// A mutex would:
//   1. Acquire a kernel synchronisation object (a futex on Linux, a CRITICAL_
//      SECTION on Windows) — this may deschedule the thread if contended.
//   2. Execute the integer increment.
//   3. Release the kernel object.
//
// That is 50–100× more work for the same net effect, and worse, it creates
// false contention: a blocked_requests increment on thread A would stall a
// simultaneous cache_hits increment on thread B, even though the two counters
// are completely independent variables.
//
// A mutex makes sense when you need to protect a *section* — multiple
// statements that must appear atomic as a group.  Here there is no section;
// each counter is independent.  atomic is the right tool.
//
// ── Memory order choice ───────────────────────────────────────────────────────
// All increments use memory_order_relaxed.  This is intentional:
//
//   - Relaxed guarantees only atomicity of the single operation — no ordering
//     constraints relative to other memory accesses.
//   - That is exactly what we need: we don't care whether "total_requests was
//     incremented" is visible to another thread *before or after* any other
//     write.  Stats are sampled periodically; a few nanoseconds of reordering
//     in how they appear to the printer thread is irrelevant.
//   - On x86, relaxed compiles identically to seq_cst for integer fetch_add
//     (the bus lock is always emitted), but the compiler is allowed to reorder
//     the surrounding code more freely — a small throughput gain on other
//     architectures (ARM, RISC-V) where relaxed avoids memory barriers.
//
// ── Snapshot semantics ────────────────────────────────────────────────────────
// snapshot() loads each counter with relaxed and returns a plain struct of
// uint64_t values.  The snapshot is not a consistent point-in-time read of all
// five counters simultaneously — a request could be counted in total_requests
// but not yet in allowed_requests if a context switch occurs between the two
// increments.  For a monitoring dashboard that prints every few seconds, this
// is perfectly acceptable.  If you needed a fully consistent snapshot you
// would need a single mutex covering all five loads, at which point you've
// negated all the atomic benefits for the hot path as well.
// ─────────────────────────────────────────────────────────────────────────────

#include <atomic>
#include <cstdint>
#include <ostream>

namespace aegis {

struct AtomicStats {
    // ── Counters — all zero-initialised ──────────────────────────────────
    std::atomic<std::uint64_t> total_requests{0};
    std::atomic<std::uint64_t> allowed_requests{0};   // passed rate limiter
    std::atomic<std::uint64_t> blocked_requests{0};   // HTTP 429 responses
    std::atomic<std::uint64_t> cache_hits{0};
    std::atomic<std::uint64_t> cache_misses{0};

    // Non-copyable — atomics cannot be copied.  Pass by shared_ptr.
    AtomicStats()                              = default;
    AtomicStats(const AtomicStats&)            = delete;
    AtomicStats& operator=(const AtomicStats&) = delete;

    // ── snapshot() ───────────────────────────────────────────────────────
    // Returns a plain (non-atomic) copy of all counters at approximately
    // the same point in time.  Useful for logging without holding any lock.
    struct Snapshot {
        std::uint64_t total_requests;
        std::uint64_t allowed_requests;
        std::uint64_t blocked_requests;
        std::uint64_t cache_hits;
        std::uint64_t cache_misses;
    };

    Snapshot snapshot() const noexcept {
        return {
            total_requests  .load(std::memory_order_relaxed),
            allowed_requests.load(std::memory_order_relaxed),
            blocked_requests.load(std::memory_order_relaxed),
            cache_hits      .load(std::memory_order_relaxed),
            cache_misses    .load(std::memory_order_relaxed),
        };
    }

    // ── print() ──────────────────────────────────────────────────────────
    // Writes a human-readable one-liner to `os`.
    void print(std::ostream& os) const {
        auto s = snapshot();
        os << "[stats] total=" << s.total_requests
           << " allowed=" << s.allowed_requests
           << " blocked=" << s.blocked_requests
           << " cache_hits=" << s.cache_hits
           << " cache_misses=" << s.cache_misses
           << "\n";
    }
};

} // namespace aegis
