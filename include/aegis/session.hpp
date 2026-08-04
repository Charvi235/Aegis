#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// aegis/session.hpp
//
// HTTP session — one per accepted TCP connection.
//
// Stage 6 changes:
//   - Session receives backend_host + backend_port at construction.
//   - make_response() is gone; replaced by an async pipeline:
//       on_read() → check rate limit + cache → proxy_request() or send cached
//   - proxy_request() creates a Proxy, calls fetch(), and the callback
//     on_proxy_response() stores the result in the cache and writes it back
//     to the client.
//   - Non-GET methods cause a cache eviction so stale GET responses are
//     invalidated when the backend state changes.
//
// ── Why on_read now calls proxy_request() instead of do_write() directly? ────
// Stages 2–5 built the response synchronously in make_response() and called
// do_write() immediately.  That worked for local logic (rate limit, cache).
// Proxying is inherently async (network round-trip to the backend), so we
// can't block waiting for it.  Instead on_read() either:
//   a) serves from cache (sync → do_write immediately), or
//   b) launches an async Proxy fetch, whose callback calls do_write().
// Both paths converge at do_write(), so the write/keep-alive logic is unchanged.
// ─────────────────────────────────────────────────────────────────────────────

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
    // Called when there's a cache miss (or a non-GET that bypasses the cache).
    // Launches a Proxy::fetch() and wires the result back to do_write().
    void proxy_request();

    // Callback invoked by Proxy::fetch() when the backend responds.
    void on_proxy_response(beast::error_code                  ec,
                           http::response<http::string_body>  backend_response);

    // ── Helpers ──────────────────────────────────────────────────────────
    http::response<http::string_body>
    make_rate_limit_response(double retry_after_secs);

    http::response<http::string_body>
    make_error_response(http::status status, std::string_view message);

    static std::string cache_key(const http::request<http::string_body>& req);

    // ── Members ──────────────────────────────────────────────────────────
    tcp::socket                        socket_;
    beast::flat_buffer                 buffer_;
    http::request<http::string_body>   request_;
    std::shared_ptr<RateLimiter>       rate_limiter_;
    std::shared_ptr<ResponseCache>     cache_;
    std::string                        backend_host_;
    std::string                        backend_port_;
};

} // namespace aegis
