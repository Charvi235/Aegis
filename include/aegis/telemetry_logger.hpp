#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// aegis/telemetry_logger.hpp
//
// The CONSUMER side of the Producer-Consumer telemetry pipeline — MongoDB
// version.
//
// This is the SAME architecture as the file-based version: worker threads
// (producers) push events into an in-memory TelemetryQueue, and ONE
// dedicated background thread (the consumer) drains it. Only the
// consumer's write target changed — instead of an ofstream writing JSON
// lines to a file, it now inserts a BSON document into a MongoDB
// collection.
//
// ── Why this swap doesn't touch the producer side at all ────────────────────
// Session::log_telemetry() still just calls telemetry_->log(event), which
// still just does queue_.push(event) — a fast in-memory operation. Worker
// threads have ZERO knowledge that MongoDB exists. This is the whole point
// of decoupling producers from the consumer: the I/O backend can change
// completely (file -> MongoDB -> Kafka -> anything) without touching a
// single line of request-handling code.
//
// ── Why batch inserts instead of one insert_one() per event ─────────────────
// A network round-trip to mongod (even on localhost) costs roughly 0.5-2ms.
// If traffic is bursty, calling insert_one() for every single event means
// the consumer thread spends most of its time waiting on network I/O
// rather than draining the queue, and the queue can back up under load.
// Instead, we drain up to BATCH_SIZE events (or whatever's available) and
// send them as one insert_many() call — this amortizes the round-trip cost
// across many documents.
//
// ── Why the consumer thread never blocks producers ───────────────────────────
// Even though insert_many() can take a few milliseconds under load, that
// latency is entirely absorbed by the consumer thread. Producers only ever
// touch queue_.push(), which is a mutex lock + push + notify — microseconds,
// regardless of how slow MongoDB is being at that moment.
// ─────────────────────────────────────────────────────────────────────────────

#include "aegis/telemetry_queue.hpp"

#include <mongocxx/client.hpp>
#include <mongocxx/instance.hpp>
#include <mongocxx/uri.hpp>
#include <bsoncxx/builder/stream/document.hpp>

#include <iostream>
#include <thread>
#include <vector>
#include <chrono>

namespace aegis {

class TelemetryLogger {
public:
    // uri              e.g. "mongodb://localhost:27017"
    // db_name          e.g. "aegis"
    // collection_name  e.g. "telemetry"
    TelemetryLogger(const std::string& uri,
                    const std::string& db_name,
                    const std::string& collection_name)
        : client_{mongocxx::uri{uri}}
        , db_{client_[db_name]}
        , collection_{db_[collection_name]}
    {
        // mongocxx::instance must be constructed exactly ONCE per process,
        // before any other mongocxx/bsoncxx type is used, and must outlive
        // all of them. A static local here guarantees "construct on first
        // use, destroy last" without the caller having to remember to do
        // it in main().

        worker_ = std::thread{&TelemetryLogger::consume_loop, this};
        std::cout << "[telemetry] Connected to MongoDB: " << uri
                  << " db=" << db_name << " collection=" << collection_name
                  << "\n";
    }

    TelemetryLogger(const TelemetryLogger&)            = delete;
    TelemetryLogger& operator=(const TelemetryLogger&) = delete;

    ~TelemetryLogger()
    {
        queue_.shutdown();
        if (worker_.joinable())
            worker_.join();
    }

    // ── log() — called by PRODUCER threads (Session, on the hot path) ──────
    // Unchanged from the file-based version: push to the in-memory queue,
    // nothing else. No MongoDB call ever happens on a worker thread.
    void log(TelemetryEvent event)
    {
        queue_.push(std::move(event));
    }

private:
    static constexpr std::size_t BATCH_SIZE = 50;

    // ── consume_loop() — runs on the single dedicated consumer thread ──────
    void consume_loop()
    {
        std::vector<TelemetryEvent> batch;
        batch.reserve(BATCH_SIZE);

        TelemetryEvent event;
        while (queue_.pop(event)) {
            batch.push_back(std::move(event));

            // Drain any additional events already waiting, up to
            // BATCH_SIZE, without blocking — this naturally adapts:
            // low traffic -> small/frequent batches, high traffic ->
            // full batches, fewer round-trips per event.
            while (batch.size() < BATCH_SIZE && queue_.try_pop(event)) {
                batch.push_back(std::move(event));
            }

            flush_batch(batch);
            batch.clear();
        }

        // Final drain: pop() returned false (shutdown + empty), but there
        // may still be events queued if shutdown raced with a burst.
        // try_pop() is non-blocking so this loop terminates immediately
        // once truly empty.
        while (queue_.try_pop(event)) {
            batch.push_back(std::move(event));
        }
        if (!batch.empty()) {
            flush_batch(batch);
        }
    }

    // The ONLY place network I/O happens in the whole telemetry path.
    void flush_batch(const std::vector<TelemetryEvent>& batch)
    {
        if (batch.empty()) return;

        using bsoncxx::builder::stream::document;
        using bsoncxx::builder::stream::finalize;

        std::vector<bsoncxx::document::value> docs;
        docs.reserve(batch.size());

        for (const auto& e : batch) {
            docs.push_back(document{}
                << "client_ip"   << e.client_ip
                << "path"        << e.path
                << "status_code" << e.status_code
                << "latency_ms"  << e.latency_ms
                << "timestamp"   << bsoncxx::types::b_date{e.timestamp}
                << finalize);
        }

        try {
            collection_.insert_many(docs);
        } catch (const std::exception& ex) {
            // A MongoDB outage should never crash the gateway or block
            // request handling — it only means telemetry for this batch
            // is lost. Log to stderr and keep going.
            std::cerr << "[telemetry] MongoDB insert failed: "
                      << ex.what() << " (" << docs.size()
                      << " events dropped)\n";
        }
    }

        // Must be declared FIRST — C++ initializes members in declaration
    // order regardless of the constructor initializer list's order.
    // mongocxx::instance must exist before any mongocxx::client is
    // constructed, so it goes first here.
    mongocxx::instance      instance_;
    TelemetryQueue          queue_;
    mongocxx::client        client_;
    mongocxx::database      db_;
    mongocxx::collection    collection_;
    std::thread             worker_;
};

} // namespace aegis
