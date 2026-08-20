#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// aegis/server.hpp
//
// Server owns all shared, long-lived resources and passes them into Sessions.
// Stage 7 adds AtomicStats (shared_ptr passed to every Session) and a
// dedicated stats-printer thread that wakes every stats_interval_s seconds.
// ─────────────────────────────────────────────────────────────────────────────

#include "aegis/atomic_stats.hpp"
#include "aegis/lru_cache.hpp"
#include "aegis/rate_limiter.hpp"
#include "aegis/telemetry_logger.hpp"
#include <boost/asio.hpp>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace aegis {

using ResponseCache = LruCache<std::string, std::string>;

class Server {
public:
    // port             – listen port
    // num_threads      – worker threads (io_context pool)
    // rl_capacity      – token bucket burst size per IP
    // rl_refill_rate   – tokens/second per IP
    // cache_capacity   – max cached responses
    // backend_host     – upstream hostname or IP
    // backend_port     – upstream port string (e.g. "9090")
    // stats_interval_s – seconds between stats log lines (0 = disabled)
    explicit Server(std::uint16_t port,
                    std::size_t   num_threads      = std::thread::hardware_concurrency(),
                    double        rl_capacity      = 10.0,
                    double        rl_refill_rate   = 5.0,
                    std::size_t   cache_capacity   = 256,
                    std::string   backend_host     = "localhost",
                    std::string   backend_port     = "9090",
                                        std::uint32_t stats_interval_s = 10,
                    std::string   mongo_uri        = "mongodb://localhost:27017",
                    std::string   mongo_db         = "aegis",
                    std::string   mongo_collection = "telemetry");
    void run();
    void stop();

    // Read-only access to the live stats (useful for tests / HTTP /metrics
    // endpoints you might add later).
    const AtomicStats& stats() const { return *stats_; }

private:
    void do_accept();

    // Runs on stats_thread_: prints a snapshot every stats_interval_s_.
    // Exits when stop_stats_ is set to true.
    void stats_loop();

    // ── Asio core ────────────────────────────────────────────────────────
    boost::asio::io_context        io_ctx_;
    boost::asio::ip::tcp::acceptor acceptor_;
    boost::asio::executor_work_guard<
        boost::asio::io_context::executor_type> work_guard_;

    // ── Server config ─────────────────────────────────────────────────────
    std::uint16_t port_;
    std::size_t   num_threads_;
    std::uint32_t stats_interval_s_;

    // ── Worker threads (io_ctx runners) ──────────────────────────────────
    std::vector<std::thread> threads_;

    // ── Shared per-request resources ─────────────────────────────────────
    std::shared_ptr<RateLimiter>   rate_limiter_;
    std::shared_ptr<ResponseCache> cache_;
    std::shared_ptr<AtomicStats>   stats_;             // Stage 7
    std::shared_ptr<TelemetryLogger> telemetry_;     // Stage 8       // Stage 7

    std::string backend_host_;
    std::string backend_port_;

    // ── Stats printer thread ──────────────────────────────────────────────
    // stop_stats_ is set to true in stop() before joining stats_thread_.
    // Using atomic<bool> so the stats_loop can read it without a mutex
    // (the loop just polls; no condition variable needed for this use case).
    std::atomic<bool> stop_stats_{false};
    std::thread       stats_thread_;
};

} // namespace aegis
