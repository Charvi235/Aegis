// ─────────────────────────────────────────────────────────────────────────────
// src/session.cpp
//
// Stage 9 addition — GET /stats admin endpoint:
//   Intercepted at the very top of on_read(), before the rate limiter and
//   before the total_requests counter, so polling from the dashboard never
//   burns tokens or skews the metrics being observed.
//   Response is JSON with CORS headers so a browser on localhost:5173 can
//   fetch it from localhost:8080 without a proxy.
//   An OPTIONS preflight is also handled here (browsers send this first for
//   cross-origin requests with a JSON content-type).
//
// Stage 7 additions (marked ── Stage 7 ──):
//   total_requests   incremented once per successfully parsed request
//   blocked_requests incremented when rate limiter denies (→ 429)
//   allowed_requests incremented when rate limiter permits
//   cache_hits       incremented on a GET cache HIT (before do_write)
//   cache_misses     incremented on a GET cache MISS (before proxy_request)
//
// All increments use fetch_add(1, memory_order_relaxed) — see
// atomic_stats.hpp for the full rationale.
//
// Request flow:
//
//   do_read()
//     └─ http::async_read()
//         └─ on_read()
//             ├─ OPTIONS /stats?         → do_write(204 + CORS preflight)
//             ├─ GET /stats?             → do_write(200 JSON + CORS)
//             ├─ rate limit denied?      → do_write(429)
//             ├─ GET + cache HIT?        → do_write(cached 200)
//             └─ otherwise              → proxy_request()
//                                            └─ Proxy::fetch()
//                                                └─ on_proxy_response()
//                                                    ├─ error?  → do_write(502)
//                                                    ├─ GET 200? → cache_.put()
//                                                    └─ do_write(backend response)
//                                                        └─ on_write()
//                                                            └─ keep-alive or close
// ─────────────────────────────────────────────────────────────────────────────

#include "aegis/session.hpp"

#include <cmath>
#include <iostream>
#include <sstream>
#include <string>

namespace aegis {

// ── Constructor ──────────────────────────────────────────────────────────────

Session::Session(tcp::socket                    socket,
                 std::shared_ptr<RateLimiter>   rate_limiter,
                 std::shared_ptr<ResponseCache> cache,
                 std::shared_ptr<AtomicStats>   stats,
                 std::shared_ptr<TelemetryLogger> telemetry,
                 std::string                    backend_host,
                 std::string                    backend_port)
    : socket_{std::move(socket)}
    , buffer_{}
    , request_{}
    , rate_limiter_{std::move(rate_limiter)}
    , cache_{std::move(cache)}
    , stats_{std::move(stats)}
    , telemetry_{std::move(telemetry)}
    , backend_host_{std::move(backend_host)}
    , backend_port_{std::move(backend_port)}
{}

// ── Public ───────────────────────────────────────────────────────────────────

void Session::start()
{
    do_read();
}

// ── Step 1: read client request ──────────────────────────────────────────────

void Session::do_read()
{
    request_ = {};
    request_start_ = std::chrono::steady_clock::now();
    http::async_read(
        socket_, buffer_, request_,
        [self = shared_from_this()](beast::error_code ec, std::size_t n)
        {
            self->on_read(ec, n);
        }
    );
}

// ── Step 2: decide what to do with the request ────────────────────────────────

void Session::on_read(beast::error_code ec, std::size_t /*n*/)
{
    // ── Client closed the connection cleanly ──────────────────────────────
    if (ec == http::error::end_of_stream) {
        do_close();
        return;
    }

    // ── Any other read error (malformed HTTP, connection reset, etc.) ─────
    // Close explicitly to release the OS fd immediately.
    // Note: we do NOT count malformed/incomplete reads in total_requests —
    // only successfully parsed HTTP messages that go through the full pipeline
    // are meaningful traffic to measure.
    if (ec) {
        std::cerr << "[session] Read error: " << ec.message() << "\n";
        do_close();
        return;
    }

    // ── Stage 9: intercept admin /stats route ────────────────────────────
    // Must come BEFORE total_requests increment and BEFORE rate limiter so:
    //   (a) dashboard polling doesn't consume tokens or trigger 429s, and
    //   (b) the counters reflect real gateway traffic, not monitoring noise.
    if (try_handle_stats()) return;

    // ── Stage 7: count every successfully parsed request ─────────────────
    stats_->total_requests.fetch_add(1, std::memory_order_relaxed);

    // ── Log the request — guard remote_endpoint() against a race ─────────
    std::string client_ip;
    try {
        auto ep   = socket_.remote_endpoint();
        client_ip = ep.address().to_string();
        client_ip_ = client_ip;
        std::cout << "[session] " << request_.method_string()
                  << " " << request_.target()
                  << " from " << ep << "\n";
    } catch (const boost::system::system_error& e) {
        std::cerr << "[session] Could not read remote endpoint: "
                  << e.what() << " — closing\n";

        do_close();
        return;
    }

    // ── Rate limit ────────────────────────────────────────────────────────
    if (!rate_limiter_->is_allowed(client_ip)) {
        double wait = rate_limiter_->retry_after(client_ip);

        // ── Stage 7 ──────────────────────────────────────────────────────
        stats_->blocked_requests.fetch_add(1, std::memory_order_relaxed);

        std::cout << "[session] Rate limited: " << client_ip
                  << " — retry in " << wait << "s\n";
                  log_telemetry(429);
        do_write(make_rate_limit_response(wait));
        return;
    }

    // ── Stage 7: request cleared the rate limiter ─────────────────────────
    stats_->allowed_requests.fetch_add(1, std::memory_order_relaxed);

    // ── Cache check (GET only) ────────────────────────────────────────────
    const bool is_get = (request_.method() == http::verb::get);

    if (is_get) {
        auto cached = cache_->get(cache_key(request_));
        if (cached.has_value()) {
            // ── Stage 7 ──────────────────────────────────────────────────
            stats_->cache_hits.fetch_add(1, std::memory_order_relaxed);

            std::cout << "[session] Cache HIT: " << cache_key(request_) << "\n";

            http::response<http::string_body> res{
                http::status::ok, request_.version()};
            res.set(http::field::server,       "Aegis/0.1");
            res.set(http::field::content_type, "text/plain");
            res.set("X-Cache", "HIT");
            res.keep_alive(request_.keep_alive());
            
            res.body() = std::move(cached.value());
            res.prepare_payload();
            log_telemetry(200);
            do_write(std::move(res));
            return;
        }

        // ── Stage 7 ──────────────────────────────────────────────────────
        stats_->cache_misses.fetch_add(1, std::memory_order_relaxed);

        std::cout << "[session] Cache MISS: " << cache_key(request_) << "\n";
    }

    // ── Mutating methods — evict stale cache entries ──────────────────────
    if (!is_get) {
        const std::string evict_key = "GET " + std::string{request_.target()};
        cache_->evict(evict_key);
    }

    // ── Forward to backend ────────────────────────────────────────────────
    proxy_request();
}

// ── Proxy path ────────────────────────────────────────────────────────────────

void Session::proxy_request()
{
    http::request<http::string_body> forwarded = request_;

    auto& ioc = static_cast<net::io_context&>(
        socket_.get_executor().context());

    auto proxy = std::make_shared<Proxy>(ioc, backend_host_, backend_port_);

    proxy->fetch(
        std::move(forwarded),
        [self = shared_from_this(), proxy](
            beast::error_code                 ec,
            http::response<http::string_body> backend_response)
        {
            self->on_proxy_response(ec, std::move(backend_response));
        }
    );
}

void Session::on_proxy_response(beast::error_code                  ec,
                                http::response<http::string_body>  backend_res)
{
   if (ec) {
        std::cerr << "[session] Proxy error: " << ec.message() << "\n";
        log_telemetry(502);
        do_write(make_error_response(
            http::status::bad_gateway,
            "Gateway error: could not reach backend\r\n"));
        return;
    }
    std::cout << "[session] Backend responded " << backend_res.result_int()
              << " for " << request_.method_string()
              << " " << request_.target() << "\n";

    const bool is_get_ok = (request_.method() == http::verb::get)
                         && (backend_res.result() == http::status::ok);

    if (is_get_ok) {
        cache_->put(cache_key(request_), backend_res.body());
    }

    http::response<http::string_body> res{
        backend_res.result(),
        request_.version()
    };
    res.set(http::field::server, "Aegis/0.1");
    res.set("X-Cache", "MISS");

    if (backend_res.count(http::field::content_type)) {
        res.set(http::field::content_type,
                backend_res[http::field::content_type]);
    } else {
        res.set(http::field::content_type, "text/plain");
    }

    res.keep_alive(request_.keep_alive());
    res.body() = std::move(backend_res.body());
    res.prepare_payload();
    log_telemetry(res.result_int());
    do_write(std::move(res));
}

// ── Write and loop ────────────────────────────────────────────────────────────

void Session::do_write(http::response<http::string_body> response)
{
    bool keep_alive = response.keep_alive();
    auto sp = std::make_shared<http::response<http::string_body>>(
                  std::move(response));

    http::async_write(
        socket_, *sp,
        [self = shared_from_this(), sp, keep_alive]
        (beast::error_code ec, std::size_t n)
        {
            self->on_write(ec, n, keep_alive);
        }
    );
}

void Session::on_write(beast::error_code ec, std::size_t /*n*/, bool keep_alive)
{
    if (ec) {
        std::cerr << "[session] Write error: " << ec.message() << "\n";
        do_close();
        return;
    }
    if (keep_alive) {
        do_read();
    } else {
        do_close();
    }
}

// ── Stage 9: /stats admin endpoint ───────────────────────────────────────────
//
// ── Why this route bypasses the rate limiter and cache ───────────────────────
// The rate limiter exists to protect the backend from malicious traffic.
// GET /stats never touches the backend — it reads from shared memory
// (AtomicStats) and returns instantly.  Applying rate limiting to the
// dashboard's polling would mean the dashboard itself could trigger 429s,
// which would (a) look broken and (b) increment blocked_requests, polluting
// the very metrics the dashboard displays.  Bypassing is the correct choice
// for any internal admin / health-check route.
//
// ── CORS headers — why they're needed ────────────────────────────────────────
// The React dashboard runs on http://localhost:5173 (Vite dev server) and
// fetches from http://localhost:8080.  Different ports = different "origins"
// in the browser security model.  Without CORS headers the browser blocks
// the response before JavaScript can read it.  The four headers we add are:
//
//   Access-Control-Allow-Origin: *
//     Permit any origin.  Safe for a local-only admin endpoint.
//     In production you'd restrict this to the dashboard's domain.
//
//   Access-Control-Allow-Methods: GET, OPTIONS
//     Only allow safe read-only methods on this route.
//
//   Access-Control-Allow-Headers: Content-Type
//     Required because the browser sends Content-Type in its preflight.
//
//   Access-Control-Max-Age: 86400
//     Cache the preflight result for 24 hours, so the browser doesn't
//     repeat the OPTIONS round-trip on every poll interval.
//
// ── OPTIONS preflight ─────────────────────────────────────────────────────────
// Before a cross-origin GET (or any request with a non-simple header), the
// browser sends an OPTIONS request to ask "is this allowed?".  We must
// respond 204 No Content with the CORS headers, or the actual GET will
// be blocked before it's even sent.

bool Session::try_handle_stats()
{
    // Only intercept the /stats path — everything else falls through.
    const auto target = std::string{request_.target()};
    if (target != "/stats") return false;

    const auto method = request_.method();

    // ── Handle CORS preflight ─────────────────────────────────────────────
    if (method == http::verb::options) {
        http::response<http::string_body> res{
            http::status::no_content, request_.version()};
        res.set(http::field::server, "Aegis/0.1");
        add_cors_headers(res);
        res.keep_alive(request_.keep_alive());
        res.prepare_payload();
        do_write(std::move(res));
        return true;
    }

    // ── Only GET is allowed beyond this point ─────────────────────────────
    if (method != http::verb::get) {
        do_write(make_error_response(
            http::status::method_not_allowed,
            "Only GET is supported on /stats\r\n"));
        return true;
    }

    // ── Build JSON from atomic snapshot ──────────────────────────────────
    auto snap = stats_->snapshot();

    std::ostringstream json;
    json << "{"
         << "\"total_requests\":"   << snap.total_requests   << ","
         << "\"allowed_requests\":" << snap.allowed_requests << ","
         << "\"blocked_requests\":" << snap.blocked_requests << ","
         << "\"cache_hits\":"       << snap.cache_hits       << ","
         << "\"cache_misses\":"     << snap.cache_misses
         << "}";

    http::response<http::string_body> res{
        http::status::ok, request_.version()};
    res.set(http::field::server,       "Aegis/0.1");
    res.set(http::field::content_type, "application/json");
    add_cors_headers(res);
    res.keep_alive(request_.keep_alive());
    res.body() = json.str();
    res.prepare_payload();
    do_write(std::move(res));
    return true;
}

void Session::add_cors_headers(http::response<http::string_body>& res) const
{
    res.set("Access-Control-Allow-Origin",  "*");
    res.set("Access-Control-Allow-Methods", "GET, OPTIONS");
    res.set("Access-Control-Allow-Headers", "Content-Type");
    res.set("Access-Control-Max-Age",       "86400");
}

// ── Explicit close helper ─────────────────────────────────────────────────────
// ── Stage 8: telemetry logging ────────────────────────────────────────────
//
// This is the ONLY telemetry-related code that runs on a worker thread.
// It builds a small struct and calls telemetry_->log(), which just pushes
// to an in-memory queue — no disk I/O happens here. The actual file write
// happens later, on TelemetryLogger's own dedicated consumer thread.
void Session::log_telemetry(int status_code)
{
    if (!telemetry_) return;   // telemetry disabled (nullptr) — no-op

    auto now = std::chrono::steady_clock::now();
    double latency_ms =
        std::chrono::duration<double, std::milli>(now - request_start_).count();

    TelemetryEvent event;
    event.client_ip   = client_ip_;
    event.path        = std::string{request_.target()};
    event.status_code = status_code;
    event.latency_ms  = latency_ms;
    event.timestamp   = std::chrono::system_clock::now();

    telemetry_->log(std::move(event));
}

void Session::do_close()
{
    beast::error_code ignored;
    socket_.shutdown(tcp::socket::shutdown_both, ignored);
    socket_.close(ignored);
}

// ── Helper response builders ──────────────────────────────────────────────────

http::response<http::string_body>
Session::make_rate_limit_response(double retry_after_secs)
{
    http::response<http::string_body> res{
        http::status::too_many_requests, request_.version()};
    res.set(http::field::server,       "Aegis/0.1");
    res.set(http::field::content_type, "text/plain");
    auto wait = static_cast<long>(std::ceil(retry_after_secs));
    res.set(http::field::retry_after, std::to_string(wait));
    res.body() = "Rate limit exceeded. Retry after "
               + std::to_string(wait) + " second(s).\r\n";
    res.keep_alive(false);
    res.prepare_payload();
    return res;
}

http::response<http::string_body>
Session::make_error_response(http::status status, std::string_view message)
{
    http::response<http::string_body> res{status, request_.version()};
    res.set(http::field::server,       "Aegis/0.1");
    res.set(http::field::content_type, "text/plain");
    res.keep_alive(false);
    res.body() = std::string{message};
    res.prepare_payload();
    return res;
}

std::string Session::cache_key(const http::request<http::string_body>& req)
{
    return std::string{req.method_string()} + " " + std::string{req.target()};
}

} // namespace aegis
