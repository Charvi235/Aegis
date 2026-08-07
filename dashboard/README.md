# Aegis Dashboard

A dark-themed React dashboard that visualises live gateway metrics from the
Aegis API gateway. Polls `GET /stats` every 2 seconds and renders:

- **Stat cards** — Total Requests, Allowed, Blocked (429s), Cache Hit Rate
- **Live traffic chart** — area chart showing allowed vs blocked requests per
  poll interval, last 30 data points
- **Allowed vs Blocked donut** — cumulative split since gateway start
- **Cache performance bar chart** — total hits vs misses
- **Connection status indicator** — pulsing green dot when the backend is
  reachable; offline banner with dimmed values when it isn't

---

## Prerequisites

| Requirement | Version |
|---|---|
| Node.js | 18 or later |
| npm | 9 or later |
| Aegis gateway | running on `localhost:8080` |

---

## Setup

```powershell
# From the Aegis repo root:
cd dashboard
npm install
npm run dev
```

Then open **http://localhost:5173** in your browser.

---

## Start the gateway first

The dashboard reads from `http://localhost:8080/stats`. Start the gateway
before (or after) opening the dashboard — it handles the offline case
gracefully.

```powershell
# Terminal 1 — mock backend (serves local files over HTTP)
python -m http.server 9090

# Terminal 2 — Aegis gateway
# Arguments: port threads rl_capacity rl_refill_rate cache_capacity backend_host backend_port stats_interval_s
.\build\Debug\aegis.exe 8080 2 5 1 256 localhost 9090 10
```

| Argument | Value | Meaning |
|---|---|---|
| `8080` | port | Aegis listens here; dashboard polls this |
| `2` | threads | 2 Asio worker threads |
| `5` | rl_capacity | Burst of 5 tokens per IP |
| `1` | rl_refill_rate | 1 token/second — easy to trigger 429s |
| `256` | cache_capacity | Up to 256 cached GET responses |
| `localhost 9090` | backend | Python mock backend |
| `10` | stats_interval_s | Print stats to console every 10 s |

Generate traffic to see the charts update:

```powershell
# Fire 20 rapid requests (triggers both allowed and 429 blocked)
1..20 | ForEach-Object {
    Invoke-WebRequest -Uri http://localhost:8080/README.md `
        -SkipHttpErrorCheck -UseBasicParsing | Out-Null
}
```

---

## Project structure

```
dashboard/
├── index.html          # Vite entry point
├── vite.config.js      # Vite config (React plugin, port 5173)
├── package.json
└── src/
    ├── main.jsx        # React root mount
    ├── App.jsx         # Full dashboard — polling, state, all charts
    ├── App.css         # Component styles
    └── index.css       # Global reset, CSS custom properties (dark theme)
```

---

## Tech choices

| Library | Why |
|---|---|
| **Vite + React 18** | Fast HMR, zero config, standard for modern React |
| **recharts** | Composable SVG charts that integrate cleanly with React state; no canvas complexity |
| **lucide-react** | Consistent, lightweight icon set used in tools like Linear and Vercel |

---

## Why polling instead of WebSockets?

**Short answer:** polling is the right fit for this project's scope, and
WebSockets would add complexity with no real benefit here.

**Longer answer for the interview:**

WebSockets make sense when the *server* needs to push events to the client
unpredictably and at high frequency — think a chat app, a collaborative
editor, or a trade ticker where latency matters. They require the server to
maintain a persistent connection per client and implement a push mechanism
(Asio async_write on a long-lived socket, or a broadcast queue).

Here the dashboard only needs a snapshot every 2 seconds. The data doesn't
change faster than that in a meaningful way, and a 2-second-old counter value
is perfectly acceptable for a monitoring panel. Polling with `fetch` gives us:

- **Zero server-side changes** — the `/stats` endpoint is a plain HTTP GET
  that the gateway already handles in its existing async pipeline
- **Stateless server** — the gateway doesn't need to track dashboard clients
  or maintain per-connection state
- **Free reconnection** — if the gateway restarts, the next poll just succeeds;
  a WebSocket would need explicit reconnect logic with backoff
- **Simpler client code** — `setInterval` + `fetch` is a dozen lines; a
  WebSocket client with reconnect, error handling, and message parsing is
  considerably more

The tradeoff is that polling creates a small, regular HTTP overhead (one
tiny JSON request every 2 seconds). At this scale — one dashboard client,
one gateway — that's completely negligible.

If you were building a dashboard for 1000 concurrent viewers, or if the
gateway needed to push alerts the instant a threshold was crossed (not within
2 seconds), WebSockets would be worth the added complexity.

---

## CORS — why it's needed

The React dev server runs on `http://localhost:5173`; the gateway runs on
`http://localhost:8080`. Different ports = different *origins* in the browser
security model (same-origin policy). Without CORS headers, the browser
silently blocks the fetch response before JavaScript can read it — even
though both servers are on the same machine.

The gateway adds four headers to every `/stats` response:

```
Access-Control-Allow-Origin:  *
Access-Control-Allow-Methods: GET, OPTIONS
Access-Control-Allow-Headers: Content-Type
Access-Control-Max-Age:       86400
```

The browser also sends an `OPTIONS` preflight before the first cross-origin
GET. The gateway handles this too, responding `204 No Content` with the same
CORS headers. `Max-Age: 86400` tells the browser to cache the preflight
result for 24 hours, so subsequent polls skip the OPTIONS round-trip entirely.

`Allow-Origin: *` is appropriate for a local-only admin endpoint. In
production you would restrict it to the dashboard's actual domain.
