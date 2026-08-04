#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// aegis/server.hpp
//
// Declares the TCP server.  The server owns:
//   - one boost::asio::io_context  (the I/O event loop)
//   - a configurable pool of std::threads that each call io_context::run()
//   - one tcp::acceptor             (listens for incoming connections)
//
// Design note — why share one io_context across N threads?
//   Boost.Asio guarantees that io_context::run() is thread-safe to call from
//   multiple threads simultaneously.  Each thread picks up the next ready
//   completion handler from the shared queue, giving us free load-balancing
//   with no extra work.  The alternative (one io_context per thread) gives
//   better cache locality but requires manual work-stealing; the shared model
//   is simpler and perfectly adequate for a portfolio project.
// ─────────────────────────────────────────────────────────────────────────────

#include <boost/asio.hpp>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace aegis {

class Server {
public:
    // port        – TCP port to listen on (e.g. 8080)
    // num_threads – how many worker threads drive io_context::run()
    //               Defaults to hardware_concurrency so the build works
    //               without any tuning, but callers can override.
    explicit Server(std::uint16_t port,
                    std::size_t   num_threads =
                        std::thread::hardware_concurrency());

    // Start listening and block until the server is stopped.
    void run();

    // Signal the io_context to stop (safe to call from any thread).
    void stop();

private:
    // Begin an async accept cycle.  Each accepted connection immediately
    // schedules the next accept, so the acceptor is always ready.
    void do_accept();

    boost::asio::io_context          io_ctx_;
    boost::asio::ip::tcp::acceptor   acceptor_;

    // Keep io_context alive while threads are running even when there is
    // temporarily nothing to do (no pending async ops).  Without this guard
    // io_context::run() would return immediately on an idle cycle.
    boost::asio::executor_work_guard<
        boost::asio::io_context::executor_type> work_guard_;

    std::uint16_t port_;
    std::size_t   num_threads_;
    std::vector<std::thread> threads_;
};

} // namespace aegis
