import { useState, useEffect, useCallback, useRef } from 'react'
import {
  AreaChart, Area, BarChart, Bar,
  XAxis, YAxis, CartesianGrid, Tooltip, ResponsiveContainer, Cell,
  PieChart, Pie, Legend,
} from 'recharts'
import {
  Activity, Shield, ShieldOff, Database,
  Zap, RefreshCw, WifiOff, TrendingUp,
} from 'lucide-react'
import './App.css'

// ── Config ────────────────────────────────────────────────────────────────────
const STATS_URL      = 'http://localhost:8080/stats'
const POLL_INTERVAL  = 2000   // ms
const HISTORY_LENGTH = 30     // data points kept in the sparkline

// ── Utility ───────────────────────────────────────────────────────────────────
function fmtNum(n) {
  if (n === null || n === undefined) return '—'
  if (n >= 1_000_000) return (n / 1_000_000).toFixed(1) + 'M'
  if (n >= 1_000)     return (n / 1_000).toFixed(1) + 'K'
  return n.toString()
}

function calcHitRate(hits, misses) {
  const total = hits + misses
  if (total === 0) return null
  return ((hits / total) * 100).toFixed(1)
}

// ── Custom tooltip for recharts ───────────────────────────────────────────────
function ChartTooltip({ active, payload, label }) {
  if (!active || !payload?.length) return null
  return (
    <div className="chart-tooltip">
      {payload.map((p) => (
        <div key={p.name} className="chart-tooltip-row">
          <span className="chart-tooltip-dot" style={{ background: p.color }} />
          <span className="chart-tooltip-label">{p.name}</span>
          <span className="chart-tooltip-value">{p.value}</span>
        </div>
      ))}
    </div>
  )
}

// ── Animated stat number ──────────────────────────────────────────────────────
function AnimatedValue({ value, formatter = fmtNum }) {
  const [display, setDisplay] = useState(value)
  const prevRef = useRef(value)

  useEffect(() => {
    if (value !== prevRef.current) {
      prevRef.current = value
      setDisplay(value)
    }
  }, [value])

  return (
    <span className="stat-value" key={display}>
      {formatter(display)}
    </span>
  )
}

// ── Single stat card ──────────────────────────────────────────────────────────
function StatCard({ icon: Icon, label, value, formatter, accent, sublabel, offline }) {
  return (
    <div className={`stat-card${offline ? ' stat-card--offline' : ''}`} data-accent={accent}>
      <div className="stat-card-header">
        <div className={`stat-card-icon stat-card-icon--${accent}`}>
          <Icon size={16} strokeWidth={2} />
        </div>
        <span className="stat-card-label">{label}</span>
      </div>
      <AnimatedValue value={value} formatter={formatter} />
      {sublabel && <span className="stat-card-sublabel">{sublabel}</span>}
    </div>
  )
}

// ── Status indicator ──────────────────────────────────────────────────────────
function StatusBadge({ online, lastUpdated }) {
  return (
    <div className={`status-badge status-badge--${online ? 'online' : 'offline'}`}>
      <span className="status-dot" />
      <span className="status-text">
        {online
          ? `Live · updated ${lastUpdated}`
          : 'Backend unreachable'}
      </span>
    </div>
  )
}

// ── Main App ──────────────────────────────────────────────────────────────────
export default function App() {
  // Live data from the last successful poll
  const [stats, setStats]     = useState(null)
  const [online, setOnline]   = useState(null)   // null = initial/unknown
  const [lastUpdated, setLastUpdated] = useState('—')

  // Rolling history for the sparkline: array of { t, rps, allowed, blocked }
  const [history, setHistory] = useState([])
  const prevStatsRef = useRef(null)
  const pollRef      = useRef(null)

  const poll = useCallback(async () => {
    try {
      const res  = await fetch(STATS_URL, { signal: AbortSignal.timeout(3000) })
      if (!res.ok) throw new Error(`HTTP ${res.status}`)
      const data = await res.json()

      const now = new Date()
      const timeLabel = now.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' })

      // Compute requests-per-interval delta from previous snapshot
      const prev   = prevStatsRef.current
      const delta  = prev
        ? {
            rps:     Math.max(0, data.total_requests   - prev.total_requests),
            allowed: Math.max(0, data.allowed_requests - prev.allowed_requests),
            blocked: Math.max(0, data.blocked_requests - prev.blocked_requests),
          }
        : { rps: 0, allowed: 0, blocked: 0 }

      prevStatsRef.current = data

      setStats(data)
      setOnline(true)
      setLastUpdated(timeLabel)
      setHistory(h => {
        const next = [...h, { t: timeLabel, ...delta }]
        return next.length > HISTORY_LENGTH ? next.slice(-HISTORY_LENGTH) : next
      })
    } catch {
      setOnline(false)
    }
  }, [])

  useEffect(() => {
    poll()
    pollRef.current = setInterval(poll, POLL_INTERVAL)
    return () => clearInterval(pollRef.current)
  }, [poll])

  const hitRate = stats
    ? calcHitRate(stats.cache_hits, stats.cache_misses)
    : null

  // Data for the allowed/blocked donut
  const donutData = stats
    ? [
        { name: 'Allowed', value: stats.allowed_requests },
        { name: 'Blocked', value: stats.blocked_requests },
      ]
    : []

  const isOffline = online === false

  return (
    <div className="shell">

      {/* ── Top bar ───────────────────────────────────────────────────── */}
      <header className="topbar">
        <div className="topbar-left">
          <div className="logo">
            <Zap size={18} className="logo-icon" />
            <span className="logo-text">Aegis</span>
          </div>
          <span className="logo-divider" />
          <span className="logo-sub">Gateway Dashboard</span>
        </div>
        <div className="topbar-right">
          <StatusBadge online={online === true} lastUpdated={lastUpdated} />
        </div>
      </header>

      {/* ── Offline banner ────────────────────────────────────────────── */}
      {isOffline && (
        <div className="offline-banner">
          <WifiOff size={14} />
          <span>Cannot reach <code>localhost:8080/stats</code> — start the gateway to see live data</span>
        </div>
      )}

      <main className="content">

        {/* ── Stat cards row ────────────────────────────────────────── */}
        <section className="cards-grid">
          <StatCard
            icon={Activity}
            label="Total Requests"
            value={stats?.total_requests ?? null}
            accent="indigo"
            sublabel="since gateway start"
            offline={isOffline}
          />
          <StatCard
            icon={Shield}
            label="Allowed"
            value={stats?.allowed_requests ?? null}
            accent="green"
            sublabel="passed rate limiter"
            offline={isOffline}
          />
          <StatCard
            icon={ShieldOff}
            label="Blocked (429)"
            value={stats?.blocked_requests ?? null}
            accent="red"
            sublabel="rate limit exceeded"
            offline={isOffline}
          />
          <StatCard
            icon={Database}
            label="Cache Hit Rate"
            value={hitRate}
            formatter={v => v !== null ? `${v}%` : '—'}
            accent="amber"
            sublabel={stats ? `${fmtNum(stats.cache_hits)} hits · ${fmtNum(stats.cache_misses)} misses` : 'no data yet'}
            offline={isOffline}
          />
        </section>

        {/* ── Charts row ────────────────────────────────────────────── */}
        <section className="charts-grid">

          {/* Live traffic sparkline */}
          <div className="chart-card chart-card--wide">
            <div className="chart-card-header">
              <TrendingUp size={14} className="chart-card-icon" />
              <span className="chart-card-title">Live Traffic</span>
              <span className="chart-card-sub">requests per {POLL_INTERVAL / 1000}s interval · last {HISTORY_LENGTH} points</span>
            </div>
            <div className="chart-body">
              {history.length < 2 ? (
                <div className="chart-empty">
                  <RefreshCw size={20} className="spin" />
                  <span>Collecting data…</span>
                </div>
              ) : (
                <ResponsiveContainer width="100%" height={220}>
                  <AreaChart data={history} margin={{ top: 8, right: 16, left: -16, bottom: 0 }}>
                    <defs>
                      <linearGradient id="gradAllowed" x1="0" y1="0" x2="0" y2="1">
                        <stop offset="5%"  stopColor="#6366f1" stopOpacity={0.3} />
                        <stop offset="95%" stopColor="#6366f1" stopOpacity={0}   />
                      </linearGradient>
                      <linearGradient id="gradBlocked" x1="0" y1="0" x2="0" y2="1">
                        <stop offset="5%"  stopColor="#ef4444" stopOpacity={0.25} />
                        <stop offset="95%" stopColor="#ef4444" stopOpacity={0}    />
                      </linearGradient>
                    </defs>
                    <CartesianGrid strokeDasharray="3 3" stroke="rgba(255,255,255,0.05)" vertical={false} />
                    <XAxis
                      dataKey="t"
                      tick={{ fill: '#4b4b5e', fontSize: 11 }}
                      tickLine={false}
                      axisLine={false}
                      interval="preserveStartEnd"
                    />
                    <YAxis
                      tick={{ fill: '#4b4b5e', fontSize: 11 }}
                      tickLine={false}
                      axisLine={false}
                      allowDecimals={false}
                      width={32}
                    />
                    <Tooltip content={<ChartTooltip />} />
                    <Area
                      type="monotone"
                      dataKey="allowed"
                      name="Allowed"
                      stroke="#6366f1"
                      strokeWidth={2}
                      fill="url(#gradAllowed)"
                      dot={false}
                      activeDot={{ r: 4, fill: '#6366f1' }}
                    />
                    <Area
                      type="monotone"
                      dataKey="blocked"
                      name="Blocked"
                      stroke="#ef4444"
                      strokeWidth={2}
                      fill="url(#gradBlocked)"
                      dot={false}
                      activeDot={{ r: 4, fill: '#ef4444' }}
                    />
                  </AreaChart>
                </ResponsiveContainer>
              )}
            </div>
          </div>

          {/* Allowed vs Blocked donut */}
          <div className="chart-card">
            <div className="chart-card-header">
              <Shield size={14} className="chart-card-icon" />
              <span className="chart-card-title">Allowed vs Blocked</span>
            </div>
            <div className="chart-body chart-body--center">
              {!stats || (stats.allowed_requests === 0 && stats.blocked_requests === 0) ? (
                <div className="chart-empty">
                  <RefreshCw size={20} className="spin" />
                  <span>No traffic yet</span>
                </div>
              ) : (
                <ResponsiveContainer width="100%" height={220}>
                  <PieChart>
                    <Pie
                      data={donutData}
                      cx="50%"
                      cy="45%"
                      innerRadius={60}
                      outerRadius={88}
                      paddingAngle={3}
                      dataKey="value"
                      strokeWidth={0}
                    >
                      <Cell fill="#6366f1" />
                      <Cell fill="#ef4444" />
                    </Pie>
                    <Tooltip content={<ChartTooltip />} />
                    <Legend
                      iconType="circle"
                      iconSize={8}
                      formatter={(value) => (
                        <span style={{ color: '#8b8b9e', fontSize: 12 }}>{value}</span>
                      )}
                    />
                  </PieChart>
                </ResponsiveContainer>
              )}
            </div>
          </div>

          {/* Cache hits vs misses bar chart */}
          <div className="chart-card">
            <div className="chart-card-header">
              <Database size={14} className="chart-card-icon" />
              <span className="chart-card-title">Cache Performance</span>
            </div>
            <div className="chart-body">
              {!stats || (stats.cache_hits === 0 && stats.cache_misses === 0) ? (
                <div className="chart-empty">
                  <RefreshCw size={20} className="spin" />
                  <span>No cache activity yet</span>
                </div>
              ) : (
                <ResponsiveContainer width="100%" height={220}>
                  <BarChart
                    data={[
                      { name: 'Hits',   value: stats.cache_hits },
                      { name: 'Misses', value: stats.cache_misses },
                    ]}
                    margin={{ top: 8, right: 16, left: -16, bottom: 0 }}
                    barSize={48}
                  >
                    <CartesianGrid strokeDasharray="3 3" stroke="rgba(255,255,255,0.05)" vertical={false} />
                    <XAxis
                      dataKey="name"
                      tick={{ fill: '#4b4b5e', fontSize: 12 }}
                      tickLine={false}
                      axisLine={false}
                    />
                    <YAxis
                      tick={{ fill: '#4b4b5e', fontSize: 11 }}
                      tickLine={false}
                      axisLine={false}
                      allowDecimals={false}
                      width={36}
                    />
                    <Tooltip content={<ChartTooltip />} />
                    <Bar dataKey="value" radius={[4, 4, 0, 0]}>
                      <Cell fill="#6366f1" />
                      <Cell fill="#f59e0b" />
                    </Bar>
                  </BarChart>
                </ResponsiveContainer>
              )}
            </div>
          </div>

        </section>

      </main>

      <footer className="footer">
        Aegis API Gateway · polling every {POLL_INTERVAL / 1000}s · Stage&nbsp;9
      </footer>

    </div>
  )
}
