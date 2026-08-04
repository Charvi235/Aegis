#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// aegis/server.hpp
//
// Server owns all shared, long-lived resources and passes them into Sessions.
// Stage 6 adds backend_host and backend_port.
// ─────────────────────────────────────────────────────────────────────────────

#include "aegis/lru_cache.hpp"
#include "aegis/rate_limiter.hpp"

#include <boost/asio.hpp>
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
    // num_threads      – worker threads
    // rl_capacity      – token bucket burst size per IP
    // rl_refill_rate   – tokens/second per IP
    // cache_capacity   – max cached responses
    // backend_host     – upstream hostname or IP
    // backend_port     – upstream port (string, e.g. "9090")
    explicit Server(std::uint16_t port,
                    std::size_t   num_threads    = std::thread::hardware_concurrency(),
                    double        rl_capacity    = 10.0,
                    double        rl_refill_rate  = 5.0,
                    std::size_t   cache_capacity  = 256,
                    std::string   backend_host    = "localhost",
                    std::string   backend_port    = "9090");

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

    std::shared_ptr<RateLimiter>   rate_limiter_;
    std::shared_ptr<ResponseCache> cache_;

    std::string backend_host_;
    std::string backend_port_;
};

} // namespace aegis
