#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// aegis/server.hpp
//
// The Server owns all shared, long-lived resources:
//   - io_context + worker thread pool
//   - tcp::acceptor
//   - RateLimiter (shared across all sessions via shared_ptr)
//
// Each accepted connection becomes a Session; the Server hands it a
// shared_ptr to the RateLimiter so it can enforce per-IP limits without
// knowing anything about how the limiter stores its state.
// ─────────────────────────────────────────────────────────────────────────────

#include "aegis/rate_limiter.hpp"

#include <boost/asio.hpp>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace aegis {

class Server {
public:
    // port            – TCP port to listen on
    // num_threads     – worker threads driving io_context::run()
    // rl_capacity     – token bucket capacity per client IP
    // rl_refill_rate  – tokens/second refill rate per client IP
    explicit Server(std::uint16_t port,
                    std::size_t   num_threads   = std::thread::hardware_concurrency(),
                    double        rl_capacity   = 10.0,
                    double        rl_refill_rate = 5.0);

    void run();
    void stop();

private:
    void do_accept();

    boost::asio::io_context        io_ctx_;
    boost::asio::ip::tcp::acceptor acceptor_;
    boost::asio::executor_work_guard<
        boost::asio::io_context::executor_type> work_guard_;

    std::uint16_t          port_;
    std::size_t            num_threads_;
    std::vector<std::thread> threads_;

    // Shared ownership: the Server and every active Session both hold a
    // reference.  The RateLimiter is destroyed only after every session has
    // finished — no dangling pointer possible.
    std::shared_ptr<RateLimiter> rate_limiter_;
};

} // namespace aegis
