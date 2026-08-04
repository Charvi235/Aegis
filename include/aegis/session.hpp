#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// aegis/session.hpp
//
// HTTP session — one per accepted TCP connection.
//
// Stage 5 changes:
//   - Session receives a shared_ptr<LruCache<string,string>> alongside the
//     rate limiter.
//   - make_response() checks the cache before doing work:
//       cache hit  → return the cached body directly (HTTP 200).
//       cache miss → proceed normally, store the result on the way out.
//   - Only GET 200 responses are cached (standard HTTP cacheability rule).
//   - The cache key is "METHOD target" (e.g. "GET /api/status").
//
// ── Why cache at the session layer, not in the rate limiter? ─────────────────
// The cache sits after the rate limiter in the pipeline:
//   rate limit check → cache lookup → [fetch/proxy] → cache store
// A rate-limited request never reaches the cache — it's turned away first.
// This means 429 responses are never cached, which is correct.
// ─────────────────────────────────────────────────────────────────────────────

#include "aegis/lru_cache.hpp"
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

// The cache stores serialised response bodies keyed by "METHOD target".
using ResponseCache = LruCache<std::string, std::string>;

class Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket                   socket,
            std::shared_ptr<RateLimiter>  rate_limiter,
            std::shared_ptr<ResponseCache> cache);

    void start();

private:
    void do_read();
    void on_read(beast::error_code ec, std::size_t bytes_transferred);
    void do_write(http::response<http::string_body> response);
    void on_write(beast::error_code ec, std::size_t bytes_transferred, bool keep_alive);

    http::response<http::string_body>
    make_response(const http::request<http::string_body>& req);

    http::response<http::string_body>
    make_rate_limit_response(const http::request<http::string_body>& req,
                             double retry_after_secs);

    // Build the cache lookup key for a request.
    static std::string cache_key(const http::request<http::string_body>& req);

    tcp::socket                        socket_;
    beast::flat_buffer                 buffer_;
    http::request<http::string_body>   request_;
    std::shared_ptr<RateLimiter>       rate_limiter_;
    std::shared_ptr<ResponseCache>     cache_;
};

} // namespace aegis
