// ─────────────────────────────────────────────────────────────────────────────
// src/session.cpp
//
// Stage 6: full reverse proxy pipeline.
//
// Request flow:
//
//   do_read()
//     └─ http::async_read()
//         └─ on_read()
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
#include <string>

namespace aegis {

// ── Constructor ──────────────────────────────────────────────────────────────

Session::Session(tcp::socket                    socket,
                 std::shared_ptr<RateLimiter>   rate_limiter,
                 std::shared_ptr<ResponseCache> cache,
                 std::string                    backend_host,
                 std::string                    backend_port)
    : socket_{std::move(socket)}
    , buffer_{}
    , request_{}
    , rate_limiter_{std::move(rate_limiter)}
    , cache_{std::move(cache)}
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
    if (ec == http::error::end_of_stream) {
        beast::error_code ignored;
        socket_.shutdown(tcp::socket::shutdown_both, ignored);
        return;
    }
    if (ec) {
        std::cerr << "[session] Read error: " << ec.message() << "\n";
        return;
    }

    std::cout << "[session] " << request_.method_string()
              << " " << request_.target()
              << " from " << socket_.remote_endpoint() << "\n";

    // ── Rate limit ────────────────────────────────────────────────────────
    const std::string client_ip =
        socket_.remote_endpoint().address().to_string();

    if (!rate_limiter_->is_allowed(client_ip)) {
        double wait = rate_limiter_->retry_after(client_ip);
        std::cout << "[session] Rate limited: " << client_ip
                  << " — retry in " << wait << "s\n";
        do_write(make_rate_limit_response(wait));
        return;
    }

    // ── Cache check (GET only) ────────────────────────────────────────────
    const bool is_get = (request_.method() == http::verb::get);

    if (is_get) {
        auto cached = cache_->get(cache_key(request_));
        if (cached.has_value()) {
            std::cout << "[session] Cache HIT: " << cache_key(request_) << "\n";

            http::response<http::string_body> res{
                http::status::ok, request_.version()};
            res.set(http::field::server,       "Aegis/0.1");
            res.set(http::field::content_type, "text/plain");
            res.set("X-Cache", "HIT");
            res.keep_alive(request_.keep_alive());
            res.body() = std::move(cached.value());
            res.prepare_payload();
            do_write(std::move(res));
            return;
        }
        std::cout << "[session] Cache MISS: " << cache_key(request_) << "\n";
    }

    // ── Mutating methods — evict stale cache entries ──────────────────────
    // If a POST/PUT/PATCH/DELETE arrives for the same path as a cached GET,
    // the backend state has changed; evict so the next GET re-fetches.
    //
    // We use a synthetic GET key because that's how the cached entry was
    // stored.  This is a best-effort heuristic — a production gateway would
    // use cache-control headers and ETags for more precise invalidation.
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
    // Build a copy of the request to forward.  We copy (not move) because
    // request_ is a member we may need for logging after the proxy returns.
    http::request<http::string_body> forwarded = request_;

    // The Proxy needs the io_context to create its own socket and resolver.
    // socket_.get_executor() returns the executor (which carries the
    // io_context reference) without exposing io_context directly.
    // net::get_associated_executor is idiomatic Asio for this.
    auto& ioc = static_cast<net::io_context&>(
        socket_.get_executor().context());

    auto proxy = std::make_shared<Proxy>(ioc, backend_host_, backend_port_);

    proxy->fetch(
        std::move(forwarded),
        // Capture self so the Session stays alive for the backend round-trip.
        // Capture proxy so it stays alive until the callback fires.
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
        do_write(make_error_response(
            http::status::bad_gateway,
            "Gateway error: could not reach backend\r\n"));
        return;
    }

    std::cout << "[session] Backend responded " << backend_res.result_int()
              << " for " << request_.method_string()
              << " " << request_.target() << "\n";

    // ── Cache store: only successful GET responses ────────────────────────
    // We cache the response body, not the full serialised message, so that
    // when we serve from cache we can reconstruct a proper response with
    // our own headers (server name, X-Cache, etc.) rather than forwarding
    // the backend's headers verbatim.
    //
    // Why not cache the full response?
    //   - Some backend headers are hop-by-hop (Connection, Transfer-Encoding)
    //     and should not be forwarded to clients.
    //   - Headers like Date change every response; caching them would serve
    //     stale dates.
    //   - Caching only the body is simpler and sufficient for a portfolio demo.
    const bool is_get_ok = (request_.method() == http::verb::get)
                         && (backend_res.result() == http::status::ok);

    if (is_get_ok) {
        cache_->put(cache_key(request_), backend_res.body());
    }

    // ── Build the response to send to the client ──────────────────────────
    // We forward the backend's status and body, but set our own headers so
    // the client sees "Aegis" as the server and we control hop-by-hop fields.
    http::response<http::string_body> res{
        backend_res.result(),
        request_.version()
    };
    res.set(http::field::server, "Aegis/0.1");
    res.set("X-Cache", "MISS");

    // Forward content-type from backend if present; default to text/plain.
    if (backend_res.count(http::field::content_type)) {
        res.set(http::field::content_type,
                backend_res[http::field::content_type]);
    } else {
        res.set(http::field::content_type, "text/plain");
    }

    res.keep_alive(request_.keep_alive());
    res.body() = std::move(backend_res.body());
    res.prepare_payload();

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
        return;
    }
    if (keep_alive) {
        do_read();
    } else {
        beast::error_code ignored;
        socket_.shutdown(tcp::socket::shutdown_both, ignored);
    }
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
