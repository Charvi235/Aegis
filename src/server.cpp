// ─────────────────────────────────────────────────────────────────────────────
// src/server.cpp
//
// TCP server implementation.  The server's only job is to:
//   1. Accept incoming TCP connections.
//   2. Wrap each connected socket in a Session (which handles HTTP).
//   3. Loop back to accept the next connection.
//
// All HTTP logic lives in Session; Server stays transport-only.
// ─────────────────────────────────────────────────────────────────────────────

#include "aegis/server.hpp"
#include "aegis/session.hpp"

#include <iostream>

namespace aegis {

namespace net = boost::asio;
using     tcp = net::ip::tcp;

// ── Constructor ──────────────────────────────────────────────────────────────

Server::Server(std::uint16_t port, std::size_t num_threads)
    : io_ctx_{}
    , acceptor_{io_ctx_,
                tcp::endpoint{tcp::v4(), port}}
    , work_guard_{net::make_work_guard(io_ctx_)}
    , port_{port}
    , num_threads_{num_threads == 0 ? 1 : num_threads}
{
    std::cout << "[server] Listening on port " << port_
              << " with " << num_threads_ << " worker thread(s)\n";
}

// ── Public interface ─────────────────────────────────────────────────────────

void Server::run()
{
    do_accept();

    threads_.reserve(num_threads_ - 1);
    for (std::size_t i = 0; i < num_threads_ - 1; ++i) {
        threads_.emplace_back([this] { io_ctx_.run(); });
    }

    io_ctx_.run();   // main thread contributes as the Nth worker

    for (auto& t : threads_) {
        if (t.joinable()) t.join();
    }
}

void Server::stop()
{
    work_guard_.reset();
    io_ctx_.stop();
    std::cout << "[server] Stopped\n";
}

// ── Private helpers ───────────────────────────────────────────────────────────

void Server::do_accept()
{
    auto socket = std::make_shared<tcp::socket>(io_ctx_);

    acceptor_.async_accept(*socket,
        [this, socket](const boost::system::error_code& ec)
        {
            if (!ec) {
                // Hand the socket off to a new Session and start it.
                // std::make_shared + shared_from_this inside Session ensures
                // the Session lives until all its async operations complete.
                std::make_shared<Session>(std::move(*socket))->start();

            } else if (ec == net::error::operation_aborted) {
                return;  // normal shutdown path
            } else {
                std::cerr << "[server] Accept error: " << ec.message() << "\n";
            }

            do_accept();  // always loop back
        }
    );
}

} // namespace aegis
