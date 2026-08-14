// ─────────────────────────────────────────────────────────────────────────────
// src/server.cpp
// ─────────────────────────────────────────────────────────────────────────────

#include "aegis/server.hpp"
#include "aegis/session.hpp"

#include <iostream>

namespace aegis {

namespace net = boost::asio;
using     tcp = net::ip::tcp;

Server::Server(std::uint16_t port,
               std::size_t   num_threads,
               double        rl_capacity,
               double        rl_refill_rate,
               std::size_t   cache_capacity,
               std::string   backend_host,
               std::string   backend_port,
               std::uint32_t stats_interval_s,
               std::string   telemetry_log_path)
    : io_ctx_{}
    , acceptor_{io_ctx_, tcp::endpoint{tcp::v4(), port}}
    , work_guard_{net::make_work_guard(io_ctx_)}
    , port_{port}
    , num_threads_{num_threads == 0 ? 1 : num_threads}
    , stats_interval_s_{stats_interval_s}
    , rate_limiter_{std::make_shared<RateLimiter>(rl_capacity, rl_refill_rate)}
    , cache_{std::make_shared<ResponseCache>(cache_capacity)}
    , stats_{std::make_shared<AtomicStats>()}
    , telemetry_{std::make_shared<TelemetryLogger>(telemetry_log_path)}
{
    backend_host_ = std::move(backend_host);
    backend_port_ = std::move(backend_port);

    std::cout << "[server] Listening on port " << port_
              << " | threads=" << num_threads_
              << " | rl=" << rl_capacity << " tok, " << rl_refill_rate << " tok/s"
              << " | cache=" << cache_capacity << " entries"
              << " | backend=" << backend_host_ << ":" << backend_port_
              << " | stats_interval=" << stats_interval_s_ << "s\n";
}

void Server::run()
{
    do_accept();

    // ── Start the stats printer thread (Stage 7) ──────────────────────────
    // Only start if a positive interval was requested.  This lets callers
    // pass stats_interval_s=0 to disable the output (useful in tests).
    if (stats_interval_s_ > 0) {
        stats_thread_ = std::thread([this] { stats_loop(); });
    }

    // ── Worker threads ─────────────────────────────────────────────────────
    threads_.reserve(num_threads_ - 1);
    for (std::size_t i = 0; i < num_threads_ - 1; ++i)
        threads_.emplace_back([this] { io_ctx_.run(); });

    io_ctx_.run();   // main thread also runs the io_context

    for (auto& t : threads_)
        if (t.joinable()) t.join();
}

void Server::stop()
{
    work_guard_.reset();
    io_ctx_.stop();

    // ── Shut down the stats printer thread ────────────────────────────────
    // Signal the loop to exit, then join so it drains and prints one final
    // snapshot before the process terminates.
    stop_stats_.store(true, std::memory_order_relaxed);
    if (stats_thread_.joinable()) {
        stats_thread_.join();
    }

    // Print a final stats summary on shutdown.
    std::cout << "[server] Stopped — final stats:\n";
    stats_->print(std::cout);

    std::cout << "[server] Stopped\n";
}

void Server::do_accept()
{
    auto socket = std::make_shared<tcp::socket>(io_ctx_);

    acceptor_.async_accept(*socket,
        [this, socket](const boost::system::error_code& ec)
        {
            if (!ec) {
                std::make_shared<Session>(
                    std::move(*socket),
                    rate_limiter_,
                    cache_,
                    stats_,          // Stage 7
                    telemetry_,      // Stage 8
                    backend_host_,
                    backend_port_
                )->start();
            } else if (ec == net::error::operation_aborted) {
                return;
            } else {
                std::cerr << "[server] Accept error: " << ec.message() << "\n";
            }
            do_accept();
        }
    );
}

// ── Stats printer loop (Stage 7) ─────────────────────────────────────────────
//
// Design choices:
//
//   - Dedicated thread rather than an Asio timer: the stats loop has zero
//     interaction with the io_context.  Adding an async_wait timer would
//     entangle the stats concern with the I/O loop; a plain thread is simpler
//     and easier to reason about.
//
//   - Polling with sleep_for rather than a condition_variable: the interval is
//     approximate by design — we don't need precise wakeup.  A CV would add
//     complexity (predicate, spurious wakeup handling) for no benefit here.
//     The downside is a potential stats_interval_s_ second delay on shutdown;
//     we mitigate by sleeping in short 100 ms slices and checking stop_stats_
//     after each slice.
//
//   - stop_stats_ is std::atomic<bool>: the stats thread reads it, the main
//     thread writes it.  No mutex needed — a single bool with relaxed ordering
//     is the lightest possible cross-thread flag.
void Server::stats_loop()
{
    using namespace std::chrono_literals;
    const auto slice    = 100ms;
    auto       elapsed  = std::chrono::milliseconds{0};
    const auto interval = std::chrono::seconds{stats_interval_s_};

    while (!stop_stats_.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(slice);
        elapsed += slice;

        if (elapsed >= interval) {
            stats_->print(std::cout);
            elapsed = std::chrono::milliseconds{0};
        }
    }
}

} // namespace aegis
