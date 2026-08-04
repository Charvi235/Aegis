// ─────────────────────────────────────────────────────────────────────────────
// src/main.cpp
//
// Usage:
//   ./aegis [port] [threads] [rl_capacity] [rl_refill_rate] [cache_capacity]
//            [backend_host] [backend_port]
//
// Defaults:
//   port           = 8080
//   threads        = hardware_concurrency
//   rl_capacity    = 10      (burst of 10 requests per IP)
//   rl_refill_rate = 5       (5 tokens/second sustained per IP)
//   cache_capacity = 256     (LRU response cache entries)
//   backend_host   = localhost
//   backend_port   = 9090
//
// Quick-start mock backend (Python, no dependencies):
//   python -m http.server 9090
// ─────────────────────────────────────────────────────────────────────────────

#include "aegis/server.hpp"

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

static aegis::Server* g_server = nullptr;

static void handle_signal(int /*signum*/)
{
    std::cout << "\n[main] Caught signal — shutting down\n";
    if (g_server) g_server->stop();
}

int main(int argc, char* argv[])
{
    std::uint16_t port           = 8080;
    std::size_t   num_threads    = std::thread::hardware_concurrency();
    double        rl_capacity    = 10.0;
    double        rl_refill_rate = 5.0;
    std::size_t   cache_capacity = 256;
    std::string   backend_host   = "localhost";
    std::string   backend_port   = "9090";

    try {
        if (argc >= 2) port           = static_cast<std::uint16_t>(std::stoi(argv[1]));
        if (argc >= 3) num_threads    = static_cast<std::size_t>   (std::stoi(argv[2]));
        if (argc >= 4) rl_capacity    = std::stod(argv[3]);
        if (argc >= 5) rl_refill_rate = std::stod(argv[4]);
        if (argc >= 6) cache_capacity = static_cast<std::size_t>   (std::stoi(argv[5]));
        if (argc >= 7) backend_host   = argv[6];
        if (argc >= 8) backend_port   = argv[7];
    } catch (const std::exception& e) {
        std::cerr << "[main] Bad argument: " << e.what() << "\n";
        std::cerr << "Usage: " << argv[0]
                  << " [port] [threads] [rl_capacity] [rl_refill_rate]"
                     " [cache_capacity] [backend_host] [backend_port]\n";
        return EXIT_FAILURE;
    }

    std::signal(SIGINT,  handle_signal);
    std::signal(SIGTERM, handle_signal);

    try {
        aegis::Server server{port, num_threads, rl_capacity, rl_refill_rate,
                              cache_capacity, backend_host, backend_port};
        g_server = &server;
        server.run();
        g_server = nullptr;
    } catch (const std::exception& e) {
        std::cerr << "[main] Fatal error: " << e.what() << "\n";
        return EXIT_FAILURE;
    }

    std::cout << "[main] Clean exit\n";
    return EXIT_SUCCESS;
}
