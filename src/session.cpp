// ─────────────────────────────────────────────────────────────────────────────
// src/session.cpp
//
// HTTP session implementation using Boost.Beast.
//
// Data flow for one HTTP exchange:
//
//   do_read()
//     └─ http::async_read()          [Beast reads + parses the full request]
//         └─ on_read()
//             └─ make_response()     [pure: inspect request, build response]
//             └─ do_write()
//                 └─ http::async_write()  [Beast serialises + sends response]
//                     └─ on_write()
//                         └─ if keep-alive → do_read()   [loop]
//                            else          → shutdown
// ─────────────────────────────────────────────────────────────────────────────

#include "aegis/session.hpp"

#include <iostream>

namespace aegis {

// ── Constructor ──────────────────────────────────────────────────────────────

Session::Session(tcp::socket socket)
    : socket_{std::move(socket)}
    , buffer_{}
    , request_{}
{}

// ── Public ───────────────────────────────────────────────────────────────────

void Session::start()
{
    do_read();
}

// ── Step 1: read ─────────────────────────────────────────────────────────────

void Session::do_read()
{
    // Reset the request object so previous data doesn't bleed into the next
    // read when the connection is kept alive.
    request_ = {};

    // http::async_read reads bytes from the socket into buffer_, feeds them
    // through the HTTP parser, and fires on_read when a complete message
    // (headers + body, if any) has been parsed — or on error.
    //
    // Why async_read and not async_read_some?
    //   async_read_some gives you a chunk of raw bytes; you'd have to run
    //   the parser yourself in a loop.  http::async_read does that loop for
    //   you and only calls back once the full request is ready.
    http::async_read(
        socket_,
        buffer_,
        request_,
        // Capture shared_from_this() so the Session lives until this
        // handler is invoked, even if the Server has long moved on.
        [self = shared_from_this()](beast::error_code ec,
                                    std::size_t bytes_transferred)
        {
            self->on_read(ec, bytes_transferred);
        }
    );
}

// ── Step 2: handle ───────────────────────────────────────────────────────────

void Session::on_read(beast::error_code ec, std::size_t /*bytes_transferred*/)
{
    // Connection closed cleanly by the peer — not an error we need to log.
    if (ec == http::error::end_of_stream) {
        beast::error_code ignored;
        socket_.shutdown(tcp::socket::shutdown_both, ignored);
        return;
    }

    if (ec) {
        std::cerr << "[session] Read error: " << ec.message() << "\n";
        return;
    }

    // Log the incoming request line (method + target + version).
    std::cout << "[session] " << request_.method_string()
              << " " << request_.target()
              << " HTTP/1." << (request_.version() == 11 ? "1" : "0")
              << " from " << socket_.remote_endpoint()
              << "\n";

    // Build the response (pure logic, no async), then fire the write.
    // We capture keep_alive before passing the request into make_response
    // because make_response takes the request by const-ref and we need the
    // flag to decide what to do after writing.
    bool keep_alive = request_.keep_alive();
    do_write(make_response(request_));

    // Note: keep_alive is checked inside on_write after the response is sent.
    // We don't need to store it as a member; do_write captures it via the
    // lambda chain.
    (void)keep_alive; // will be used in do_write below
}

// ── Step 3: write ────────────────────────────────────────────────────────────

void Session::do_write(http::response<http::string_body> response)
{
    // Honour the request's keep-alive preference in our response.
    // Beast needs to know this to set the Connection header correctly.
    bool keep_alive = response.keep_alive();

    // Move the response into a shared_ptr so the lambda can extend its
    // lifetime across the async write.  Beast's async_write holds a view
    // into the response (not a copy), so the response must outlive the
    // write operation.
    auto sp = std::make_shared<http::response<http::string_body>>(
                  std::move(response));

    http::async_write(
        socket_,
        *sp,
        [self = shared_from_this(), sp, keep_alive]
        (beast::error_code ec, std::size_t bytes_transferred)
        {
            self->on_write(ec, bytes_transferred, keep_alive);
        }
    );
}

// ── Step 4: after write ───────────────────────────────────────────────────────

void Session::on_write(beast::error_code ec,
                       std::size_t       /*bytes_transferred*/,
                       bool              keep_alive)
{
    if (ec) {
        std::cerr << "[session] Write error: " << ec.message() << "\n";
        return;
    }

    if (keep_alive) {
        // HTTP/1.1 persistent connection: loop back and wait for the next
        // request on the same socket.
        do_read();
    } else {
        // HTTP/1.0 or Connection: close — shut down cleanly.
        beast::error_code ignored;
        socket_.shutdown(tcp::socket::shutdown_both, ignored);
    }
}

// ── Response builder ─────────────────────────────────────────────────────────

http::response<http::string_body>
Session::make_response(const http::request<http::string_body>& req)
{
    // ── Stage 2: static "hello" response ─────────────────────────────────
    // In later stages this function will:
    //   - check the rate limiter (Stage 4) → return 429 if exhausted
    //   - consult the LRU cache (Stage 5)  → return cached response
    //   - forward to backend (Stage 6)     → relay the proxied response

    http::response<http::string_body> res{
        http::status::ok,
        req.version()       // echo back the HTTP version (10 or 11)
    };

    res.set(http::field::server, "Aegis/0.1");
    res.set(http::field::content_type, "text/plain");

    // Propagate keep-alive from the request into the response.
    // Beast will set the Connection header accordingly when serialising.
    res.keep_alive(req.keep_alive());

    res.body() = "Hello from Aegis!\r\n";
    res.prepare_body();  // sets Content-Length from body size

    return res;
}

} // namespace aegis
