#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// aegis/server.hpp
//
// Server owns all shared, long-lived resources:
//   - io_context + worker thread pool
//   - tcp::acceptor
//   - RateLimiter   (shared across sessions)
//   - ResponseCache (shared across sessions)
//
// Stage 5 adds the ResponseCache parameter.
// ─────────────────────────────────────────────────────────────────────────────

#include "aegis/lru_cache.hpp"
#include "aegis/rate_limiter.hpp"

#include <boost/asio.hpp>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace aegis {

using ResponseCache = LruCache<std::string, std::string>;

class Server {
public:
    // port             – TCP port to listen on
    // num_threads      – worker threads
    // rl_capacity      – token bucket capacity per IP
    // rl_refill_rate   – tokens/second per IP
    // cache_capacity   – max cached responses (LRU eviction)
    explicit Server(std::uint16_t port,
                    std::size_t   num_threads    = std::thread::hardware_concurrency(),
                    double        rl_capacity    = 10.0,
                    double        rl_refill_rate  = 5.0,
                    std::size_t   cache_capacity  = 256);

    void run();
    void stop();

private:
    void do_accept();

    boost::asio::io_context        io_ctx_;
    boost::asio::ip::tcp::acceptor acceptor_;
    boost::asio::executor_work_guard<
        boost::asio::io_context::executor_type> work_guard_;

    std::uint16_t            port_;
    std::size_t              num_threads_;
    std::vector<std::thread> threads_;

    std::shared_ptr<RateLimiter>    rate_limiter_;
    std::shared_ptr<ResponseCache>  cache_;
};

} // namespace aegis
