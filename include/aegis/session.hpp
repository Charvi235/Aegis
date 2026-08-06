#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// aegis/session.hpp
//
// HTTP session — one per accepted TCP connection.
//
// Stage 7 adds a shared_ptr<AtomicStats> member.  Session increments the
// relevant counter at each decision point in on_read() using fetch_add with
// memory_order_relaxed — see atomic_stats.hpp for the full rationale.
// ─────────────────────────────────────────────────────────────────────────────

#include "aegis/atomic_stats.hpp"
#include "aegis/lru_cache.hpp"
#include "aegis/proxy.hpp"
#include "aegis/rate_limiter.hpp"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <memory>
#include <string>

namespace aegis {

namespace net   = boost::asio;
namespace beast = boost::beast;
namespace http  = beast::http;
using     tcp   = net::ip::tcp;

using ResponseCache = LruCache<std::string, std::string>;

class Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket                    socket,
            std::shared_ptr<RateLimiter>   rate_limiter,
            std::shared_ptr<ResponseCache> cache,
            std::shared_ptr<AtomicStats>   stats,        // Stage 7
            std::string                    backend_host,
            std::string                    backend_port);

    void start();

private:
    // ── Client-side async pipeline ───────────────────────────────────────
    void do_read();
    void on_read(beast::error_code ec, std::size_t n);
    void do_write(http::response<http::string_body> response);
    void on_write(beast::error_code ec, std::size_t n, bool keep_alive);

    // ── Proxy path ───────────────────────────────────────────────────────
    void proxy_request();
    void on_proxy_response(beast::error_code                  ec,
                           http::response<http::string_body>  backend_response);

    // ── Helpers ──────────────────────────────────────────────────────────
    http::response<http::string_body>
    make_rate_limit_response(double retry_after_secs);

    http::response<http::string_body>
    make_error_response(http::status status, std::string_view message);

    static std::string cache_key(const http::request<http::string_body>& req);

    void do_close();

    // ── Stage 9: /stats admin endpoint helpers ────────────────────────────
    // Returns true if the request was handled internally (caller must return).
    bool try_handle_stats();
    // Adds the four CORS headers required for cross-origin browser fetch.
    // Called on every /stats response (200 and 204).
    void add_cors_headers(http::response<http::string_body>& res) const;

    // ── Members ──────────────────────────────────────────────────────────
    tcp::socket                        socket_;
    beast::flat_buffer                 buffer_;
    http::request<http::string_body>   request_;
    std::shared_ptr<RateLimiter>       rate_limiter_;
    std::shared_ptr<ResponseCache>     cache_;
    std::shared_ptr<AtomicStats>       stats_;          // Stage 7
    std::string                        backend_host_;
    std::string                        backend_port_;
};

} // namespace aegis
