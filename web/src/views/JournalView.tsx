import { Fragment, useMemo, useState } from "react"
import { useLive } from "../api/live"
import { useFills, useTrades } from "../api/trading"
import type { Fill, Trade, TradingStatus } from "../api/trading-types"
import { HBarChart } from "../charts/HBarChart"
import { TradingError } from "../components/TradingControls"
import { Empty, PageHeader, Panel, Segmented, Tile, toneOf, toneText } from "../components/ui"
import { signedPercent } from "../lib/format"
import { timestampET } from "../lib/freshness"
import { contractLabel, dailyResults, formatDuration, journalStats, monthWeeks, newYorkDate, tradeBuckets, tradeNet, type Dimension, type Side } from "../lib/journal"
import { formatMoney, signedMoney } from "../lib/trading"

const usd = (value: number) => signedMoney(value.toFixed(2))
const monthName = new Intl.DateTimeFormat("en-US", { month: "long", year: "numeric", timeZone: "UTC" })
const short = new Intl.DateTimeFormat("en-US", { month: "short", day: "numeric", hour: "numeric", minute: "2-digit", timeZone: "America/New_York" })

export function JournalView() {
  const { trading, accountScope } = useLive()
  if (!trading) return null
  return <Journal key={accountScope} trading={trading} />
}

function Journal({ trading }: { trading: TradingStatus }) {
  const [scope, setScope] = useState<"current" | "all">("current")
  const trades = useTrades(scope)
  const list = useMemo(() => trades.data?.trades ?? [], [trades.data])
  const stats = useMemo(() => journalStats(list), [list])
  if (trades.error) return <TradingError error={trades.error} />
  if (!trades.data) return <Empty>{trading.enabled ? "Loading journal…" : trading.reason ?? "Paper trading is unavailable"}</Empty>
  const pf = stats.profitFactor
  return (
    <div className="min-w-0 space-y-4">
      <PageHeader title="Journal" subtitle={`${stats.trades} closed trade${stats.trades === 1 ? "" : "s"} · ${scope === "current" ? `attempt ${trades.data.attempt}` : "all attempts"}`}>
        <Segmented label="Attempts" value={scope} onChange={setScope} options={[{ value: "current", label: "This attempt" }, { value: "all", label: "All attempts" }]} />
      </PageHeader>
      <div className="grid min-w-0 grid-cols-2 gap-3 md:grid-cols-3 xl:grid-cols-5">
        <Tile label="Net P&L (closed)" value={usd(stats.net)} tone={toneOf(stats.net)} detail={`fees ${formatMoney(stats.fees.toFixed(2))}`} />
        <Tile label="Win rate" value={stats.winRate == null ? "—" : `${(stats.winRate * 100).toFixed(1)}%`} detail={`${stats.wins} of ${stats.wins + stats.losses} decided`} />
        <Tile label="Profit factor" value={pf == null ? "—" : Number.isFinite(pf) ? pf.toFixed(2) : "∞"} detail="gross wins ÷ gross losses" />
        <Tile label="Average win" value={stats.averageWin == null ? "—" : usd(stats.averageWin)} tone="positive" />
        <Tile label="Average loss" value={stats.averageLoss == null ? "—" : usd(stats.averageLoss)} tone={stats.averageLoss == null ? "neutral" : "negative"} />
        <Tile label="Best trade" value={stats.best ? usd(tradeNet(stats.best)) : "—"} tone={stats.best ? toneOf(tradeNet(stats.best)) : "neutral"} detail={stats.best ? contractLabel(stats.best) : undefined} />
        <Tile label="Worst trade" value={stats.worst ? usd(tradeNet(stats.worst)) : "—"} tone={stats.worst ? toneOf(tradeNet(stats.worst)) : "neutral"} detail={stats.worst ? contractLabel(stats.worst) : undefined} />
        <Tile label="Trades" value={String(stats.trades)} detail={`${stats.contracts} contracts`} />
        <Tile label="Average hold" value={formatDuration(stats.averageHoldSeconds)} />
        <Tile label="Open trades" value={String(list.filter((t) => t.status === "open").length)} />
      </div>
      <Calendar trades={list} />
      <Reports trades={list} />
      <History trades={list} />
    </div>
  )
}

function Calendar({ trades }: { trades: Trade[] }) {
  const days = useMemo(() => dailyResults(trades), [trades])
  const today = newYorkDate(new Date().toISOString())?.date ?? ""
  const latest = [...days.keys()].sort().at(-1) ?? today
  const [cursor, setCursor] = useState(() => ({ year: Number(latest.slice(0, 4)) || 2026, month: Number(latest.slice(5, 7)) || 1 }))
  const weeks = monthWeeks(cursor.year, cursor.month)
  const month = [...days.values()].filter((d) => d.date.startsWith(`${cursor.year}-${String(cursor.month).padStart(2, "0")}`))
  const net = month.reduce((sum, d) => sum + d.net, 0)
  const scale = Math.max(1, ...month.map((d) => Math.abs(d.net)))
  const move = (delta: number) => setCursor(({ year, month }) => {
    const index = year * 12 + month - 1 + delta
    return { year: Math.floor(index / 12), month: (index % 12) + 1 }
  })
  return (
    <Panel title="P&L calendar" actions={<div className="flex flex-wrap items-center gap-3 text-xs">
      <span className="text-muted">Month <span className={`tabular ${toneText[toneOf(net)]}`}>{usd(net)}</span></span>
      <span className="text-muted">Profitable days <span className="tabular text-foreground">{month.filter((d) => d.net > 0).length}/{month.length}</span></span>
      <button type="button" className="trade-button" aria-label="Previous month" onClick={() => move(-1)}>‹</button>
      <span className="min-w-28 text-center font-medium">{monthName.format(Date.UTC(cursor.year, cursor.month - 1, 1))}</span>
      <button type="button" className="trade-button" aria-label="Next month" onClick={() => move(1)}>›</button>
    </div>}>
      <div className="overflow-x-auto">
        <table className="w-full min-w-[40rem] table-fixed border-separate border-spacing-1 text-xs" aria-label="Daily P&L">
          <thead><tr className="text-[10px] uppercase tracking-wide text-muted">
            {["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"].map((d) => <th key={d} className="font-normal">{d}</th>)}
            <th className="w-24 font-normal">Week</th>
          </tr></thead>
          <tbody>
            {weeks.map((week, index) => {
              const results = week.map((date) => (date ? days.get(date) : undefined))
              const weekNet = results.reduce((sum, r) => sum + (r?.net ?? 0), 0)
              const traded = results.filter(Boolean).length
              return <tr key={index}>
                {week.map((date, i) => {
                  const result = results[i]
                  const intensity = result ? Math.round(12 + 38 * Math.min(1, Math.abs(result.net) / scale)) : 0
                  return <td key={i} className={`h-16 rounded-md align-top ${date ? "border border-border/60" : ""} ${date === today ? "ring-1 ring-accent" : ""}`}
                    style={result ? { background: `color-mix(in srgb, var(${result.net >= 0 ? "--bullish" : "--bearish"}) ${intensity}%, var(--panel))` } : undefined}>
                    {date && <div className="flex h-full flex-col p-1.5">
                      <span className="text-[10px] text-muted">{Number(date.slice(8))}</span>
                      {result && <>
                        <span className="mt-auto font-medium tabular">{usd(result.net)}</span>
                        <span className="text-[10px] text-muted">{result.trades} trade{result.trades === 1 ? "" : "s"}</span>
                      </>}
                    </div>}
                  </td>
                })}
                <td className="rounded-md border border-border/60 bg-raised/40 p-1.5 align-top">
                  <div className="text-[10px] uppercase text-muted">Week {index + 1}</div>
                  <div className={`font-medium tabular ${toneText[toneOf(weekNet)]}`}>{traded ? usd(weekNet) : "$0"}</div>
                  <div className="text-[10px] text-muted">{traded} day{traded === 1 ? "" : "s"}</div>
                </td>
              </tr>
            })}
          </tbody>
        </table>
      </div>
      <p className="mt-2 text-[11px] text-muted">Closed trades by the New York date they closed.</p>
    </Panel>
  )
}

function Reports({ trades }: { trades: Trade[] }) {
  const [side, setSide] = useState<Side>("all")
  const [dimension, setDimension] = useState<Dimension>("duration")
  const buckets = tradeBuckets(trades, dimension, side)
  const by = dimension === "duration" ? "hold time" : dimension === "weekday" ? "weekday" : "month"
  return (
    <Panel title="Trade reports" actions={<>
      <Segmented label="Contracts" value={side} onChange={setSide} options={[{ value: "all", label: "All" }, { value: "call", label: "Calls" }, { value: "put", label: "Puts" }]} />
      <Segmented label="Group by" value={dimension} onChange={setDimension} options={[{ value: "duration", label: "Hold time" }, { value: "weekday", label: "Weekday" }, { value: "month", label: "Month" }]} />
    </>}>
      <div className="grid gap-6 lg:grid-cols-3">
        <div><h3 className="mb-2 text-xs text-muted">Net P&L by {by}</h3>
          <HBarChart label={`Net P&L by ${by}`} signed rows={buckets.map((b) => ({ label: b.label, value: b.trades ? b.net : null }))} format={usd} /></div>
        <div><h3 className="mb-2 text-xs text-muted">Trades by {by}</h3>
          <HBarChart label={`Trades by ${by}`} rows={buckets.map((b) => ({ label: b.label, value: b.trades || null }))} format={(v) => String(v)} /></div>
        <div><h3 className="mb-2 text-xs text-muted">Win rate by {by}</h3>
          <HBarChart label={`Win rate by ${by}`} domain={1} rows={buckets.map((b) => ({ label: b.label, value: b.winRate }))} format={(v) => `${(v * 100).toFixed(0)}%`} /></div>
      </div>
    </Panel>
  )
}

const pageSize = 25
function History({ trades }: { trades: Trade[] }) {
  const [filter, setFilter] = useState<"closed" | "open" | "all">("closed")
  const [page, setPage] = useState(0)
  const [expanded, setExpanded] = useState<string | null>(null)
  const fills = useFills().data?.fills
  const fillsById = useMemo(() => new Map((fills ?? []).map((f) => [f.id, f])), [fills])
  const rows = trades.filter((t) => filter === "all" || t.status === filter)
  const pages = Math.max(1, Math.ceil(rows.length / pageSize))
  const current = Math.min(page, pages - 1)
  const visible = rows.slice(current * pageSize, (current + 1) * pageSize)
  const net = rows.filter((t) => t.status === "closed").reduce((sum, t) => sum + tradeNet(t), 0)
  return (
    <Panel title="Trade history" actions={<>
      <span className="text-xs text-muted">Net <span className={`tabular ${toneText[toneOf(net)]}`}>{usd(net)}</span></span>
      <Segmented label="Trade status" value={filter} onChange={(v) => { setFilter(v); setPage(0) }}
        options={[{ value: "closed", label: "Closed" }, { value: "open", label: "Open" }, { value: "all", label: "All" }]} />
    </>}>
      {!rows.length ? <p className="text-sm text-muted">No {filter === "all" ? "" : `${filter} `}trades yet.</p> : <>
        <div className="max-w-full overflow-x-auto" tabIndex={0} role="region" aria-label="Trade history">
          <table className="w-full text-right text-xs tabular whitespace-nowrap">
            <thead className="text-[11px] uppercase tracking-wide text-muted"><tr>
              {["Contract", "Side", "Qty", "Opened", "Closed", "Held", "Avg open", "Avg close", "Net P&L", "Return"].map((h, i) =>
                <th key={h} scope="col" className={`px-2 py-2 font-normal ${i === 0 ? "text-left" : ""}`}>{h}</th>)}
            </tr></thead>
            <tbody>
              {visible.map((t) => <Fragment key={t.id}>
                <tr className="cursor-pointer border-t border-border/40 hover:bg-raised/50" onClick={() => setExpanded(expanded === t.id ? null : t.id)}
                  aria-expanded={expanded === t.id}>
                  <td className="px-2 py-2 text-left">
                    <span className={`mr-2 inline-block h-3 w-0.5 align-middle ${t.status === "open" ? "bg-accent" : tradeNet(t) >= 0 ? "bg-bullish" : "bg-bearish"}`} />
                    <span className="font-medium">{contractLabel(t)}</span>
                    {t.closure && <span className="ml-1 text-[10px] text-muted">({t.closure})</span>}
                  </td>
                  <td className={`px-2 py-2 ${t.direction === "long" ? "text-bullish" : "text-bearish"}`}>{t.direction}</td>
                  <td className="px-2 py-2">{t.max_quantity}</td>
                  <td className="px-2 py-2 text-muted">{short.format(Date.parse(t.opened))}</td>
                  <td className="px-2 py-2 text-muted">{t.closed ? short.format(Date.parse(t.closed)) : "open"}</td>
                  <td className="px-2 py-2">{formatDuration(t.duration_seconds)}</td>
                  <td className="px-2 py-2">{formatMoney(t.average_open)}</td>
                  <td className="px-2 py-2">{formatMoney(t.average_close)}</td>
                  <td className={`px-2 py-2 ${toneText[toneOf(t.status === "open" ? t.unrealised : t.net)]}`}>
                    {t.status === "open" ? <span title="Unrealized">{signedMoney(t.unrealised)}</span> : signedMoney(t.net)}
                  </td>
                  <td className={`px-2 py-2 ${toneText[toneOf(t.return)]}`}>{signedPercent(t.return)}</td>
                </tr>
                {expanded === t.id && <tr className="bg-raised/30"><td colSpan={10} className="px-4 py-3 text-left">
                  <TradeDetail trade={t} fills={t.fills.map((id) => fillsById.get(id)).filter((f): f is Fill => f != null)} />
                </td></tr>}
              </Fragment>)}
            </tbody>
          </table>
        </div>
        {pages > 1 && <div className="mt-3 flex items-center justify-between text-xs text-muted">
          <span>Showing {current * pageSize + 1}–{Math.min(rows.length, (current + 1) * pageSize)} of {rows.length}</span>
          <span className="flex items-center gap-2">
            <button type="button" className="trade-button" disabled={current === 0} onClick={() => setPage(current - 1)}>Previous</button>
            <span className="tabular">{current + 1} / {pages}</span>
            <button type="button" className="trade-button" disabled={current >= pages - 1} onClick={() => setPage(current + 1)}>Next</button>
          </span>
        </div>}
      </>}
    </Panel>
  )
}

function TradeDetail({ trade, fills }: { trade: Trade; fills: Fill[] }) {
  return <div className="grid gap-4 md:grid-cols-[16rem_1fr]">
    <dl className="grid grid-cols-2 gap-x-3 gap-y-1 text-xs">
      <dt className="text-muted">Entry cost</dt><dd className="tabular">{formatMoney(trade.cost)}</dd>
      <dt className="text-muted">Gross P&L</dt><dd className="tabular">{signedMoney(trade.gross)}</dd>
      <dt className="text-muted">Fees</dt><dd className="tabular">{formatMoney(trade.fees)}</dd>
      <dt className="text-muted">Contracts in / out</dt><dd className="tabular">{trade.opened_contracts} / {trade.closed_contracts}</dd>
      <dt className="text-muted">Opened</dt><dd>{timestampET(trade.opened)}</dd>
      <dt className="text-muted">Attempt</dt><dd>#{trade.attempt}</dd>
    </dl>
    <div className="max-w-full overflow-x-auto">
      <table className="w-full text-right text-xs tabular whitespace-nowrap" aria-label="Trade fills">
        <thead className="text-muted"><tr>{["Fill", "Time", "Side", "Qty", "Price", "Fee"].map((h, i) => <th key={h} className={`px-2 py-1 font-normal ${i === 0 ? "text-left" : ""}`}>{h}</th>)}</tr></thead>
        <tbody>{fills.map((f) => <tr key={f.id} className="border-t border-border/40">
          <td className="px-2 py-1 text-left">#{f.id}</td><td className="px-2 py-1 text-muted">{timestampET(f.time)}</td>
          <td className={`px-2 py-1 ${f.side === "buy" ? "text-bullish" : "text-bearish"}`}>{f.side}</td>
          <td className="px-2 py-1">{f.quantity}</td><td className="px-2 py-1">{formatMoney(f.price)}</td><td className="px-2 py-1">{formatMoney(f.fee)}</td>
        </tr>)}
        {trade.closure && <tr className="border-t border-border/40"><td className="px-2 py-1 text-left" colSpan={6}>Closed by {trade.closure} at {formatMoney(trade.average_close)}</td></tr>}
        </tbody>
      </table>
    </div>
  </div>
}
