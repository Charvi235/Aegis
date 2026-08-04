// ─────────────────────────────────────────────────────────────────────────────
// src/session.cpp
//
// Stage 5: LRU cache wired into make_response().
//
// Request flow:
//
//   do_read()
//     └─ http::async_read()
//         └─ on_read()
//             └─ make_response()
//                 ├─ rate_limiter_->is_allowed(ip)?
//                 │    NO  → 429
//                 ├─ cache_->get(key)?
//                 │    HIT  → return cached body (200, X-Cache: HIT)
//                 │    MISS → build response, store in cache, return it
//             └─ do_write()
//                 └─ http::async_write()
//                     └─ on_write()
// ─────────────────────────────────────────────────────────────────────────────

#include "aegis/session.hpp"

#include <cmath>
#include <iostream>
#include <string>

namespace aegis {

// ── Constructor ──────────────────────────────────────────────────────────────

Session::Session(tcp::socket                    socket,
                 std::shared_ptr<RateLimiter>   rate_limiter,
                 std::shared_ptr<ResponseCache> cache)
    : socket_{std::move(socket)}
    , buffer_{}
    , request_{}
    , rate_limiter_{std::move(rate_limiter)}
    , cache_{std::move(cache)}
{}

// ── Public ───────────────────────────────────────────────────────────────────

void Session::start()
{
    do_read();
}

// ── Step 1: read ─────────────────────────────────────────────────────────────

void Session::do_read()
{
    request_ = {};
    http::async_read(
        socket_, buffer_, request_,
        [self = shared_from_this()](beast::error_code ec, std::size_t n)
        {
            self->on_read(ec, n);
        }
    );
}

// ── Step 2: handle ───────────────────────────────────────────────────────────

void Session::on_read(beast::error_code ec, std::size_t /*n*/)
{
    if (ec == http::error::end_of_stream) {
        beast::error_code ignored;
        socket_.shutdown(tcp::socket::shutdown_both, ignored);
        return;
    }
    if (ec) {
        std::cerr << "[session] Read error: " << ec.message() << "\n";
        return;
    }

    std::cout << "[session] " << request_.method_string()
              << " " << request_.target()
              << " from " << socket_.remote_endpoint() << "\n";

    do_write(make_response(request_));
}

// ── Step 3: write ────────────────────────────────────────────────────────────

void Session::do_write(http::response<http::string_body> response)
{
    bool keep_alive = response.keep_alive();
    auto sp = std::make_shared<http::response<http::string_body>>(
                  std::move(response));

    http::async_write(
        socket_, *sp,
        [self = shared_from_this(), sp, keep_alive]
        (beast::error_code ec, std::size_t n)
        {
            self->on_write(ec, n, keep_alive);
        }
    );
}

// ── Step 4: after write ───────────────────────────────────────────────────────

void Session::on_write(beast::error_code ec, std::size_t /*n*/, bool keep_alive)
{
    if (ec) {
        std::cerr << "[session] Write error: " << ec.message() << "\n";
        return;
    }

    if (keep_alive) {
        do_read();
    } else {
        beast::error_code ignored;
        socket_.shutdown(tcp::socket::shutdown_both, ignored);
    }
}

// ── Response builder ─────────────────────────────────────────────────────────

std::string Session::cache_key(const http::request<http::string_body>& req)
{
    // Simple key: "METHOD target"  e.g. "GET /api/v1/status"
    // In Stage 6 this can be extended with the backend URL if different
    // backends serve different paths.
    return std::string{req.method_string()} + " " + std::string{req.target()};
}

http::response<http::string_body>
Session::make_response(const http::request<http::string_body>& req)
{
    // ── 1. Rate limit check ───────────────────────────────────────────────
    const std::string client_ip =
        socket_.remote_endpoint().address().to_string();

    if (!rate_limiter_->is_allowed(client_ip)) {
        double wait = rate_limiter_->retry_after(client_ip);
        std::cout << "[session] Rate limited: " << client_ip
                  << " — retry in " << wait << "s\n";
        return make_rate_limit_response(req, wait);
    }

    // ── 2. Cache lookup (GET requests only) ───────────────────────────────
    // We only cache GET because:
    //   - POST/PUT/DELETE mutate state; caching them would return stale data.
    //   - HEAD is safe to cache too, but omitted for simplicity.
    const bool is_get = (req.method() == http::verb::get);

    if (is_get) {
        const std::string key = cache_key(req);
        auto cached_body      = cache_->get(key);

        if (cached_body.has_value()) {
            std::cout << "[session] Cache HIT for " << key << "\n";

            http::response<http::string_body> res{http::status::ok, req.version()};
            res.set(http::field::server,       "Aegis/0.1");
            res.set(http::field::content_type, "text/plain");
            // X-Cache is a de facto standard header used by most proxies
            // (Varnish, nginx, CloudFront) to indicate cache status.
            res.set("X-Cache", "HIT");
            res.keep_alive(req.keep_alive());
            res.body() = std::move(cached_body.value());
            res.prepare_body();
            return res;
        }

        std::cout << "[session] Cache MISS for " << key << "\n";
    }

    // ── 3. Generate the response ──────────────────────────────────────────
    // Stage 2 placeholder body; Stage 6 replaces this with proxy logic.
    std::string body = "Hello from Aegis!\r\n";

    // ── 4. Store in cache if applicable ──────────────────────────────────
    // Only cache 200 GET responses.  Non-200 responses (errors, redirects)
    // are transient and shouldn't be served stale.
    if (is_get) {
        cache_->put(cache_key(req), body);
    }

    http::response<http::string_body> res{http::status::ok, req.version()};
    res.set(http::field::server,       "Aegis/0.1");
    res.set(http::field::content_type, "text/plain");
    res.set("X-Cache", "MISS");
    res.keep_alive(req.keep_alive());
    res.body() = std::move(body);
    res.prepare_body();
    return res;
}

http::response<http::string_body>
Session::make_rate_limit_response(const http::request<http::string_body>& req,
                                  double retry_after_secs)
{
    http::response<http::string_body> res{
        http::status::too_many_requests, req.version()
    };
    res.set(http::field::server,       "Aegis/0.1");
    res.set(http::field::content_type, "text/plain");

    auto wait_secs = static_cast<long>(std::ceil(retry_after_secs));
    res.set(http::field::retry_after, std::to_string(wait_secs));
    res.body() = "Rate limit exceeded. Retry after "
               + std::to_string(wait_secs) + " second(s).\r\n";
    res.keep_alive(false);
    res.prepare_body();
    return res;
}

} // namespace aegis
