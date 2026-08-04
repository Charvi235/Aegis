#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// aegis/session.hpp
//
// HTTP session — one per accepted TCP connection.
//
// Stage 4 changes:
//   - Session now receives a shared_ptr<RateLimiter> at construction.
//   - make_response() checks the rate limiter keyed by the client's IP.
//   - If the bucket is empty, it returns HTTP 429 with a Retry-After header
//     instead of the normal response.
//
// The session does NOT own the RateLimiter; it merely borrows a shared
// reference.  This makes the ownership chain clear: Server creates it,
// everyone else just uses it.
// ─────────────────────────────────────────────────────────────────────────────

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

class Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket socket, std::shared_ptr<RateLimiter> rate_limiter);

    void start();

private:
    void do_read();
    void on_read(beast::error_code ec, std::size_t bytes_transferred);
    void do_write(http::response<http::string_body> response);
    void on_write(beast::error_code ec, std::size_t bytes_transferred, bool keep_alive);

    http::response<http::string_body>
    make_response(const http::request<http::string_body>& req);

    // Build a 429 Too Many Requests response.
    // Extracted as a named function so the logic is readable and the
    // Retry-After header is set in one place.
    http::response<http::string_body>
    make_rate_limit_response(const http::request<http::string_body>& req,
                             double retry_after_secs);

    tcp::socket                       socket_;
    beast::flat_buffer                buffer_;
    http::request<http::string_body>  request_;
    std::shared_ptr<RateLimiter>      rate_limiter_;
};

} // namespace aegis
