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
//     out (to a file, or to MongoDB) — all the slow I/O happens only on
//     this one thread, never blocking request handling.
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
    // BLOCKS (sleeps) until an event is available OR shutdown() is called.
    // Returns false if the queue was shut down and is empty (time to exit).
    // Use this to wait for the *next* event when the queue is empty.
    bool pop(TelemetryEvent& out)
    {
        std::unique_lock<std::mutex> lock{mutex_};

        cv_.wait(lock, [this] { return !queue_.empty() || shutting_down_; });

        if (queue_.empty()) {
            return false;   // woke up because of shutdown, nothing left
        }

        out = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    // ── try_pop() — called by the CONSUMER thread only ──────────────────────
    // NON-BLOCKING variant: returns immediately with false if the queue is
    // currently empty, instead of sleeping. Used for opportunistic batch
    // draining — after handling one event via pop(), the consumer calls
    // try_pop() in a loop to grab any additional events that arrived in the
    // meantime (up to a batch size limit) without waiting for new ones.
    bool try_pop(TelemetryEvent& out)
    {
        std::lock_guard<std::mutex> lock{mutex_};

        if (queue_.empty()) {
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
    std::mutex                 mutex_;
    std::condition_variable    cv_;
    std::queue<TelemetryEvent> queue_;
    bool                       shutting_down_ = false;
};

} // namespace aegis
