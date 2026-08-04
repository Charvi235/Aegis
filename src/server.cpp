// ─────────────────────────────────────────────────────────────────────────────
// src/server.cpp
//
// TCP server implementation.  At this stage (Stage 1) we only:
//   1. Accept a connection.
//   2. Log the remote endpoint (IP + port).
//   3. Read whatever bytes the client sends (so the socket drains cleanly).
//   4. Close the socket and loop back to accept the next connection.
//
// HTTP parsing, rate limiting, and proxying are all added in later stages.
// ─────────────────────────────────────────────────────────────────────────────

#include "aegis/server.hpp"

#include <boost/asio.hpp>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

namespace aegis {

namespace net = boost::asio;
using     tcp = net::ip::tcp;

// ── Constructor ──────────────────────────────────────────────────────────────

Server::Server(std::uint16_t port, std::size_t num_threads)
    : io_ctx_{}
    , acceptor_{io_ctx_,
                tcp::endpoint{tcp::v4(), port}}   // bind + listen in one step
    , work_guard_{net::make_work_guard(io_ctx_)}  // prevent premature exit
    , port_{port}
    , num_threads_{num_threads == 0 ? 1 : num_threads}
    // hardware_concurrency() can return 0 on unusual platforms; guard it.
{
    // SO_REUSEADDR is set by Boost when we pass the endpoint to the acceptor
    // constructor above, so we don't need to set socket options manually.
    std::cout << "[server] Listening on port " << port_
              << " with " << num_threads_ << " worker thread(s)\n";
}

// ── Public interface ─────────────────────────────────────────────────────────

void Server::run()
{
    // Kick off the first async accept before spinning up threads.
    // If we started threads first and do_accept() wasn't called yet,
    // the work_guard keeps them alive but there's nothing to do yet —
    // harmless, but cleaner to post work first.
    do_accept();

    // Spin up N-1 additional threads; this calling thread will also call
    // io_ctx_.run() below, contributing the Nth worker.
    threads_.reserve(num_threads_ - 1);
    for (std::size_t i = 0; i < num_threads_ - 1; ++i) {
        threads_.emplace_back([this] { io_ctx_.run(); });
    }

    // The main thread joins the pool.  run() blocks here until stop() is
    // called (which triggers io_context::stop() after the work guard is
    // released).
    io_ctx_.run();

    // Wait for every worker thread to finish its current handler before
    // returning.  This ensures clean shutdown with no dangling callbacks.
    for (auto& t : threads_) {
        if (t.joinable()) t.join();
    }
}

void Server::stop()
{
    // Release the work guard first so that once all pending handlers drain,
    // io_context::run() is allowed to return naturally.
    work_guard_.reset();
    io_ctx_.stop();
    std::cout << "[server] Stopped\n";
}

// ── Private helpers ───────────────────────────────────────────────────────────

void Server::do_accept()
{
    // Create a socket for the next incoming connection.
    // We allocate it on the heap (via shared_ptr) so its lifetime is tied
    // to the lambda capture — it stays alive until the handler fires.
    auto socket = std::make_shared<tcp::socket>(io_ctx_);

    acceptor_.async_accept(*socket,
        [this, socket](const boost::system::error_code& ec)
        {
            if (!ec) {
                // Log the remote endpoint before doing anything else.
                auto remote = socket->remote_endpoint();
                std::cout << "[server] Accepted connection from "
                          << remote.address().to_string()
                          << ":" << remote.port() << "\n";

                // ── Stage 1: drain and discard the incoming bytes ─────────
                // We use a shared buffer + async_read_some to consume
                // whatever the client sent, then close.  This avoids an
                // RST (connection reset) that would appear if we closed
                // without reading, which can confuse clients and test tools.
                //
                // In Stage 2 this block is replaced by Boost.Beast HTTP
                // parsing and a proper response.
                auto buf = std::make_shared<std::array<char, 4096>>();
                socket->async_read_some(
                    net::buffer(*buf),
                    [socket, buf](const boost::system::error_code& /*ec*/,
                                  std::size_t bytes_transferred)
                    {
                        // Just acknowledge receipt in the log; ignore content.
                        std::cout << "[server] Read " << bytes_transferred
                                  << " byte(s) — closing connection\n";
                        // Graceful close: shutdown send side, then close.
                        boost::system::error_code ignored;
                        socket->shutdown(tcp::socket::shutdown_both, ignored);
                        socket->close(ignored);
                    }
                );
            } else if (ec == net::error::operation_aborted) {
                // Acceptor was closed (e.g. during shutdown). Not an error.
                return;
            } else {
                std::cerr << "[server] Accept error: " << ec.message() << "\n";
            }

            // Always loop back to accept the next connection, even after an
            // error, unless we're shutting down (handled by operation_aborted).
            do_accept();
        }
    );
}

} // namespace aegis
