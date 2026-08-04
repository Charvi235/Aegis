// ─────────────────────────────────────────────────────────────────────────────
// src/session.cpp
//
// HTTP session.  Stage 4 adds rate-limit enforcement in make_response().
//
// Request flow:
//
//   do_read()
//     └─ http::async_read()
//         └─ on_read()
//             └─ make_response()
//                 ├─ rate_limiter_->is_allowed(ip)?
//                 │    NO  → make_rate_limit_response()  [HTTP 429]
//                 │    YES → build normal response        [HTTP 200]
//             └─ do_write()
//                 └─ http::async_write()
//                     └─ on_write()
//                         └─ keep-alive loop or shutdown
// ─────────────────────────────────────────────────────────────────────────────

#include "aegis/session.hpp"

#include <cmath>      // std::ceil
#include <iostream>
#include <string>

namespace aegis {

// ── Constructor ──────────────────────────────────────────────────────────────

Session::Session(tcp::socket socket, std::shared_ptr<RateLimiter> rate_limiter)
    : socket_{std::move(socket)}
    , buffer_{}
    , request_{}
    , rate_limiter_{std::move(rate_limiter)}
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

http::response<http::string_body>
Session::make_response(const http::request<http::string_body>& req)
{
    // ── Rate limit check ─────────────────────────────────────────────────
    // Extract the client IP as a string.  remote_endpoint() is safe to call
    // here because the socket is still open (we just finished reading from it).
    const std::string client_ip = socket_.remote_endpoint()
                                         .address().to_string();

    if (!rate_limiter_->is_allowed(client_ip)) {
        double wait = rate_limiter_->retry_after(client_ip);
        std::cout << "[session] Rate limited: " << client_ip
                  << " — retry in " << wait << "s\n";
        return make_rate_limit_response(req, wait);
    }

    // ── Normal response (Stage 2 placeholder; replaced in Stage 6) ───────
    http::response<http::string_body> res{http::status::ok, req.version()};
    res.set(http::field::server, "Aegis/0.1");
    res.set(http::field::content_type, "text/plain");
    res.keep_alive(req.keep_alive());
    res.body() = "Hello from Aegis!\r\n";
    res.prepare_body();
    return res;
}

http::response<http::string_body>
Session::make_rate_limit_response(const http::request<http::string_body>& req,
                                  double retry_after_secs)
{
    http::response<http::string_body> res{
        http::status::too_many_requests,  // 429
        req.version()
    };

    res.set(http::field::server,       "Aegis/0.1");
    res.set(http::field::content_type, "text/plain");

    // Retry-After: RFC 7231 §7.1.3 — an integer number of seconds.
    // We ceiling the fractional wait so the client doesn't retry too early.
    auto wait_secs = static_cast<long>(std::ceil(retry_after_secs));
    res.set(http::field::retry_after, std::to_string(wait_secs));

    // RFC 7231 says 429 responses SHOULD include a body explaining the limit.
    res.body() = "Rate limit exceeded. Retry after "
               + std::to_string(wait_secs) + " second(s).\r\n";

    // Do NOT keep the connection alive after a 429.
    // The client should back off and reconnect; holding the connection open
    // would waste server resources and send the wrong signal.
    res.keep_alive(false);
    res.prepare_body();
    return res;
}

} // namespace aegis
