// ─────────────────────────────────────────────────────────────────────────────
// src/main.cpp
//
// Entry point.  Parses minimal CLI arguments and hands off to Server.
//
// Usage:
//   ./aegis [port] [threads]
//
// Defaults:  port=8080, threads=hardware_concurrency
// ─────────────────────────────────────────────────────────────────────────────

#include "aegis/server.hpp"

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

// Global pointer so the signal handler can reach the server.
// Using a raw pointer here is intentional: the server lives in main's stack
// frame and outlives any signal handler invocation.
static aegis::Server* g_server = nullptr;

static void handle_signal(int /*signum*/)
{
    std::cout << "\n[main] Caught signal — shutting down\n";
    if (g_server) g_server->stop();
}

int main(int argc, char* argv[])
{
    // ── Parse arguments ───────────────────────────────────────────────────
    std::uint16_t port        = 8080;
    std::size_t   num_threads = std::thread::hardware_concurrency();

    try {
        if (argc >= 2) port        = static_cast<std::uint16_t>(std::stoi(argv[1]));
        if (argc >= 3) num_threads = static_cast<std::size_t>   (std::stoi(argv[2]));
    } catch (const std::exception& e) {
        std::cerr << "[main] Bad argument: " << e.what() << "\n";
        std::cerr << "Usage: " << argv[0] << " [port] [threads]\n";
        return EXIT_FAILURE;
    }

    // ── Install signal handlers ───────────────────────────────────────────
    // SIGINT  = Ctrl-C
    // SIGTERM = kill / system shutdown
    std::signal(SIGINT,  handle_signal);
    std::signal(SIGTERM, handle_signal);

    // ── Run server ────────────────────────────────────────────────────────
    try {
        aegis::Server server{port, num_threads};
        g_server = &server;
        server.run();              // blocks until stop() is called
        g_server = nullptr;
    } catch (const std::exception& e) {
        std::cerr << "[main] Fatal error: " << e.what() << "\n";
        return EXIT_FAILURE;
    }

    std::cout << "[main] Clean exit\n";
    return EXIT_SUCCESS;
}
