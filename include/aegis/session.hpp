#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// aegis/session.hpp
//
// An HTTP session wraps one accepted TCP socket and drives the full
// request-response lifecycle for that connection:
//
//   read request  →  handle  →  write response  →  (keep-alive or close)
//
// Design — why a separate Session class?
//   In Stage 1, every connection was handled by a nest of lambdas inside
//   do_accept().  That works for a single async step, but an HTTP exchange
//   has at least four steps (read, parse, handle, write) and will grow more
//   as we add rate limiting and proxying.  Stuffing all of that into lambdas
//   produces deeply nested, hard-to-follow code.
//
//   A Session object collects the socket, the Beast flat_buffer, and the
//   parsed request into one place.  Each async step is a named member
//   function, so the flow reads linearly: do_read → on_read → do_write →
//   on_write.
//
// Design — lifetime via shared_ptr / enable_shared_from_this
//   Async handlers must guarantee the objects they reference are still alive
//   when the handler fires.  By deriving from enable_shared_from_this and
//   capturing shared_from_this() in each lambda, the Session's refcount
//   stays > 0 for as long as there is at least one pending async op.
//   When the last op completes and the lambda is destroyed, the Session is
//   automatically cleaned up — no manual delete, no dangling pointer.
//
// Design — Beast flat_buffer
//   Beast requires its own buffer type (not a raw char array) because it
//   tracks how many bytes have been consumed by the parser vs how many are
//   still pending.  flat_buffer is the simplest choice; it resizes
//   dynamically and works with both async_read and the HTTP parser.
// ─────────────────────────────────────────────────────────────────────────────

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
    // Takes ownership of the connected socket.
    explicit Session(tcp::socket socket);

    // Kick off async activity.  Must be called once after construction;
    // the Session keeps itself alive from here on via shared_from_this().
    void start();

private:
    // ── Async pipeline steps ─────────────────────────────────────────────

    // Step 1: read a complete HTTP request from the socket.
    void do_read();

    // Step 2: process the parsed request, build a response, then write it.
    // Broken into a separate function to keep on_read() short.
    void on_read(beast::error_code ec, std::size_t bytes_transferred);

    // Step 3: send the response we built.
    void do_write(http::response<http::string_body> response);

    // Step 4: after write completes, decide keep-alive or close.
    void on_write(beast::error_code ec,
                  std::size_t bytes_transferred,
                  bool keep_alive);

    // ── Helpers ──────────────────────────────────────────────────────────

    // Build the appropriate HTTP response for the request we received.
    // Extracted as a pure function (no async) so it's trivially testable.
    http::response<http::string_body>
    make_response(const http::request<http::string_body>& req);

    // ── Member data ───────────────────────────────────────────────────────
    tcp::socket                       socket_;

    // flat_buffer accumulates raw bytes between async reads.
    // Beast uses it to track parser state across partial reads.
    beast::flat_buffer                buffer_;

    // The parsed HTTP request.  We store it as a member because on_read()
    // needs to pass it to make_response() and then potentially keep it for
    // logging or rate-limiting decisions in later stages.
    http::request<http::string_body>  request_;
};

} // namespace aegis
