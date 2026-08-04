// ─────────────────────────────────────────────────────────────────────────────
// src/server.cpp
// ─────────────────────────────────────────────────────────────────────────────

#include "aegis/server.hpp"
#include "aegis/session.hpp"

#include <iostream>

namespace aegis {

namespace net = boost::asio;
using     tcp = net::ip::tcp;

Server::Server(std::uint16_t port,
               std::size_t   num_threads,
               double        rl_capacity,
               double        rl_refill_rate,
               std::size_t   cache_capacity,
               std::string   backend_host,
               std::string   backend_port)
    : io_ctx_{}
    , acceptor_{io_ctx_, tcp::endpoint{tcp::v4(), port}}
    , work_guard_{net::make_work_guard(io_ctx_)}
    , port_{port}
    , num_threads_{num_threads == 0 ? 1 : num_threads}
    , rate_limiter_{std::make_shared<RateLimiter>(rl_capacity, rl_refill_rate)}
    , cache_{std::make_shared<ResponseCache>(cache_capacity)}
    , backend_host_{std::move(backend_host)}
    , backend_port_{std::move(backend_port)}
{
    std::cout << "[server] Listening on port " << port_
              << " | threads=" << num_threads_
              << " | rl=" << rl_capacity << " tok, " << rl_refill_rate << " tok/s"
              << " | cache=" << cache_capacity << " entries"
              << " | backend=" << backend_host_ << ":" << backend_port_ << "\n";
}

void Server::run()
{
    do_accept();

    threads_.reserve(num_threads_ - 1);
    for (std::size_t i = 0; i < num_threads_ - 1; ++i)
        threads_.emplace_back([this] { io_ctx_.run(); });

    io_ctx_.run();

    for (auto& t : threads_)
        if (t.joinable()) t.join();
}

void Server::stop()
{
    work_guard_.reset();
    io_ctx_.stop();
    std::cout << "[server] Stopped\n";
}

void Server::do_accept()
{
    auto socket = std::make_shared<tcp::socket>(io_ctx_);

    acceptor_.async_accept(*socket,
        [this, socket](const boost::system::error_code& ec)
        {
            if (!ec) {
                std::make_shared<Session>(
                    std::move(*socket),
                    rate_limiter_,
                    cache_,
                    backend_host_,
                    backend_port_
                )->start();
            } else if (ec == net::error::operation_aborted) {
                return;
            } else {
                std::cerr << "[server] Accept error: " << ec.message() << "\n";
            }
            do_accept();
        }
    );
}

} // namespace aegis
