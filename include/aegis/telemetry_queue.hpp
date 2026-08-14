#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// aegis/telemetry_queue.hpp
//
// A thread-safe Producer-Consumer queue for telemetry events.
//
// Design:
//   - Multiple "producer" threads (the request-handling worker threads)
//     push events into this queue. This is a fast, in-memory operation —
//     no disk I/O, no network I/O happens on these threads.
//   - A single dedicated "consumer" thread pops events and writes them
//     out (to a file, or later to MongoDB) — all the slow I/O happens
//     only on this one thread, never blocking request handling.
//   - std::condition_variable lets the consumer thread SLEEP when the
//     queue is empty, instead of busy-polling (wasting CPU checking an
//     empty queue in a tight loop).
// ─────────────────────────────────────────────────────────────────────────────

#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <chrono>

namespace aegis {

// One telemetry event — one client request worth of data to log.
struct TelemetryEvent {
    std::string client_ip;
    std::string path;
    int status_code;
    double latency_ms;
    std::chrono::system_clock::time_point timestamp;
};

class TelemetryQueue {
public:
    // ── push() — called by PRODUCER threads (worker threads) ──────────────
    // Adds an event to the queue and wakes up the consumer thread if it's
    // sleeping. This is fast: lock, push, unlock, notify. No I/O here.
    void push(TelemetryEvent event)
    {
        {
            std::lock_guard<std::mutex> lock{mutex_};
            queue_.push(std::move(event));
        }
        cv_.notify_one();
    }

    // ── pop() — called by the CONSUMER thread only ─────────────────────────
    // Blocks (sleeps) until an event is available OR shutdown() is called.
    // Returns false if the queue was shut down and is empty (time to exit).
    bool pop(TelemetryEvent& out)
    {
        std::unique_lock<std::mutex> lock{mutex_};

        // Sleep until there's something to do: either an event arrives,
        // or we're shutting down. This avoids a busy-wait loop.
        cv_.wait(lock, [this] { return !queue_.empty() || shutting_down_; });

        if (queue_.empty()) {
            // Woke up because of shutdown, and nothing left to process.
            return false;
        }

        out = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    // ── shutdown() — called once, when the server is stopping ─────────────
    // Wakes up the consumer thread so it can notice shutting_down_ and
    // drain any remaining events before exiting cleanly.
    void shutdown()
    {
        {
            std::lock_guard<std::mutex> lock{mutex_};
            shutting_down_ = true;
        }
        cv_.notify_all();
    }

private:
    std::mutex              mutex_;
    std::condition_variable cv_;
    std::queue<TelemetryEvent> queue_;
    bool                     shutting_down_ = false;
};

} // namespace aegis