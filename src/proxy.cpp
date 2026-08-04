// ─────────────────────────────────────────────────────────────────────────────
// src/proxy.cpp
//
// Async pipeline for one upstream (backend) request:
//
//   fetch()
//     └─ do_resolve()
//         └─ async_resolve()
//             └─ on_resolve()
//                 └─ do_connect()
//                     └─ async_connect()
//                         └─ on_connect()
//                             └─ do_write()
//                                 └─ http::async_write()
//                                     └─ on_write()
//                                         └─ do_read()
//                                             └─ http::async_read()
//                                                 └─ on_read()
//                                                     └─ callback_(ec, response_)
// ─────────────────────────────────────────────────────────────────────────────

#include "aegis/proxy.hpp"

#include <iostream>

namespace aegis {

// ── Constructor ──────────────────────────────────────────────────────────────

Proxy::Proxy(net::io_context& ioc,
             std::string       backend_host,
             std::string       backend_port)
    : resolver_{ioc}
    , socket_{ioc}
    , buffer_{}
    , backend_host_{std::move(backend_host)}
    , backend_port_{std::move(backend_port)}
{}

// ── Public ───────────────────────────────────────────────────────────────────

void Proxy::fetch(http::request<http::string_body> request, Callback cb)
{
    request_  = std::move(request);
    callback_ = std::move(cb);

    // ── Host header rewrite ───────────────────────────────────────────────
    // HTTP/1.1 requires a Host header matching the server the request is
    // actually sent to.  The incoming request's Host is the gateway's own
    // address; we rewrite it to the backend's address before forwarding.
    // Without this, many backends will reject or mis-route the request.
    request_.set(http::field::host, backend_host_ + ":" + backend_port_);

    do_resolve();
}

// ── Step 1: DNS resolve ───────────────────────────────────────────────────────

void Proxy::do_resolve()
{
    // async_resolve takes a hostname + service name (or port string) and
    // produces a list of endpoints to try.  Using the resolver rather than
    // hardcoding an IP means the backend can be a DNS name (e.g. a Docker
    // service name or a load-balanced hostname) and we get the right address.
    resolver_.async_resolve(
        backend_host_,
        backend_port_,
        [self = shared_from_this()](beast::error_code ec,
                                    tcp::resolver::results_type results)
        {
            self->on_resolve(ec, std::move(results));
        }
    );
}

void Proxy::on_resolve(beast::error_code ec,
                       tcp::resolver::results_type results)
{
    if (ec) {
        std::cerr << "[proxy] Resolve error: " << ec.message() << "\n";
        callback_(ec, {});
        return;
    }
    do_connect(std::move(results));
}

// ── Step 2: TCP connect ───────────────────────────────────────────────────────

void Proxy::do_connect(tcp::resolver::results_type results)
{
    // async_connect tries each endpoint in `results` in order until one
    // succeeds or all fail.  This gives us automatic fallback if the backend
    // has multiple A records (e.g. round-robin DNS).
    net::async_connect(
        socket_,
        results,
        [self = shared_from_this()](beast::error_code ec,
                                    tcp::resolver::results_type::endpoint_type ep)
        {
            self->on_connect(ec, ep);
        }
    );
}

void Proxy::on_connect(beast::error_code ec,
                       tcp::resolver::results_type::endpoint_type /*ep*/)
{
    if (ec) {
        std::cerr << "[proxy] Connect error: " << ec.message() << "\n";
        callback_(ec, {});
        return;
    }
    do_write();
}

// ── Step 3: send the forwarded request ───────────────────────────────────────

void Proxy::do_write()
{
    http::async_write(
        socket_,
        request_,
        [self = shared_from_this()](beast::error_code ec, std::size_t n)
        {
            self->on_write(ec, n);
        }
    );
}

void Proxy::on_write(beast::error_code ec, std::size_t /*n*/)
{
    if (ec) {
        std::cerr << "[proxy] Write error: " << ec.message() << "\n";
        callback_(ec, {});
        return;
    }
    do_read();
}

// ── Step 4: read the backend's response ───────────────────────────────────────

void Proxy::do_read()
{
    // http::async_read reads the complete response (status line + headers +
    // body) in one shot.  For very large responses this buffers the whole body
    // in memory — a streaming approach would read in chunks and pipe them
    // through to the client socket simultaneously, but that requires managing
    // two concurrent async loops and is beyond our current scope.
    http::async_read(
        socket_,
        buffer_,
        response_,
        [self = shared_from_this()](beast::error_code ec, std::size_t n)
        {
            self->on_read(ec, n);
        }
    );
}

void Proxy::on_read(beast::error_code ec, std::size_t /*n*/)
{
    if (ec && ec != http::error::end_of_stream) {
        std::cerr << "[proxy] Read error: " << ec.message() << "\n";
        callback_(ec, {});
        return;
    }

    // Gracefully close the backend socket now that we have the full response.
    // We don't reuse it (no connection pool yet).
    beast::error_code ignored;
    socket_.shutdown(tcp::socket::shutdown_both, ignored);

    // Deliver the response to the caller (Session::on_proxy_response).
    callback_({}, std::move(response_));
}

} // namespace aegis
