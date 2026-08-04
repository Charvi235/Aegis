#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// aegis/proxy.hpp
//
// Proxy: forwards an incoming HTTP request to a backend server and delivers
// the backend's response back to the caller via a completion callback.
//
// ── Why a separate Proxy class? ───────────────────────────────────────────────
// Session handles the client-facing socket lifecycle (accept, read, write,
// keep-alive).  Proxy handles the backend-facing socket lifecycle (resolve,
// connect, write request, read response).  Keeping them separate means:
//   - Each class has one clear socket it owns.
//   - Session doesn't need to know about DNS resolution or backend addresses.
//   - The proxy logic can be unit-tested by calling fetch() directly without
//     standing up a full Session.
//
// ── Buffered (non-streaming) proxy ───────────────────────────────────────────
// We read the COMPLETE backend response before forwarding it to the client.
// A streaming proxy (read a chunk → write a chunk) would have lower latency
// for large responses, but is significantly more complex:
//   - You need two concurrent async loops running on the same Asio executor.
//   - Backpressure (client slower than backend) requires careful buffering.
// For a portfolio project demonstrating the core concepts, buffered is the
// right tradeoff.  The comment here documents that the choice is intentional,
// not an oversight.
//
// ── One Proxy object per request ─────────────────────────────────────────────
// Proxy is created per-request (not shared across sessions) because it owns
// a TCP socket to the backend.  Reusing a persistent backend connection would
// require a connection pool — a worthwhile future enhancement, but out of
// scope here.
//
// ── Callback design ──────────────────────────────────────────────────────────
// fetch() takes a completion callback rather than returning a future/promise.
// This keeps us in the Asio async callback model consistently — mixing
// std::future with Asio requires either blocking or a strand trick, both of
// which add complexity without benefit in a single io_context design.
// ─────────────────────────────────────────────────────────────────────────────

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <functional>
#include <memory>
#include <string>

namespace aegis {

namespace net   = boost::asio;
namespace beast = boost::beast;
namespace http  = beast::http;
using     tcp   = net::ip::tcp;

class Proxy : public std::enable_shared_from_this<Proxy> {
public:
    // Completion callback type:
    //   - error_code: non-zero if the proxy encountered a network error.
    //   - response:   the backend response (meaningful only if ec is clear).
    using Callback = std::function<
        void(beast::error_code, http::response<http::string_body>)>;

    // backend_host – hostname or IP of the backend (e.g. "localhost")
    // backend_port – port as a string (e.g. "9090")
    Proxy(net::io_context& ioc,
          std::string       backend_host,
          std::string       backend_port);

    // Send `request` to the backend and invoke `cb` with the result.
    // The Proxy keeps itself alive via shared_from_this() until `cb` fires.
    void fetch(http::request<http::string_body> request, Callback cb);

private:
    // ── Async pipeline ───────────────────────────────────────────────────
    void do_resolve();
    void on_resolve(beast::error_code ec,
                    tcp::resolver::results_type results);
    void do_connect(tcp::resolver::results_type results);
    void on_connect(beast::error_code ec,
                    tcp::resolver::results_type::endpoint_type /*ep*/);
    void do_write();
    void on_write(beast::error_code ec, std::size_t /*n*/);
    void do_read();
    void on_read(beast::error_code ec, std::size_t /*n*/);

    tcp::resolver                    resolver_;
    tcp::socket                      socket_;
    beast::flat_buffer               buffer_;

    http::request<http::string_body>  request_;
    http::response<http::string_body> response_;
    Callback                          callback_;

    std::string backend_host_;
    std::string backend_port_;
};

} // namespace aegis
