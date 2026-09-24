import { Fragment, useMemo, useState } from "react"
import { api } from "../api/client"
import { marketNow, useLive } from "../api/live"
import { useAllOrders, useFills, useRefreshTrading, useTrades } from "../api/trading"
import type { Fill, ShareTrade, Trade, TradingStatus } from "../api/trading-types"
import { HBarChart } from "../charts/HBarChart"
import { TradingError, WriteAccess, writeBlocked } from "../components/TradingControls"
import { Empty, PageHeader, Panel, Segmented, Tile, toneOf, toneText } from "../components/ui"
import { signedPercent } from "../lib/format"
import { timestampET } from "../lib/freshness"
import { contractLabel, dailyResults, formatDuration, journalLabel, journalStats, monthWeeks, newYorkDate, parseTags, shareSourceLabel, tradeBuckets, tradeNet, tradeTags, type Dimension, type JournalTrade, type Side } from "../lib/journal"
import { tradeGroups, type TradeGroup } from "../lib/positions"
import { formatMoney, signedMoney } from "../lib/trading"
import { useWriteToken } from "../lib/write-token"
import { netLabel } from "./OrdersView"

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
  const [tag, setTag] = useState("")
  const trades = useTrades(scope)
  const all = useMemo(() => trades.data?.trades ?? [], [trades.data])
  const shares = useMemo(() => trades.data?.share_trades ?? [], [trades.data])
  const tags = useMemo(() => tradeTags(all), [all])
  // Every panel reads the trades with the chosen tag; shares from exercise and
  // assignment count beside the options, and carry no tags.
  const list = useMemo(() => tag ? all.filter((t) => t.tags?.includes(tag)) : all, [all, tag])
  const entries = useMemo<JournalTrade[]>(() => tag ? list : [...list, ...shares], [list, shares, tag])
  const stats = useMemo(() => journalStats(entries), [entries])
  if (trades.error) return <TradingError error={trades.error} />
  if (!trades.data) return <Empty>{trading.enabled ? "Loading journal…" : trading.reason ?? "Paper trading is unavailable"}</Empty>
  const pf = stats.profitFactor
  return (
    <div className="min-w-0 space-y-4">
      <PageHeader title="Journal" subtitle={`${stats.trades} closed trade${stats.trades === 1 ? "" : "s"} · ${scope === "current" ? `attempt ${trades.data.attempt}` : "all attempts"}${tag ? ` · tagged ${tag}` : ""}`}>
        {(tags.length > 0 || tag) && <label className="flex items-center gap-2 text-xs text-muted">Tag
          <select className="trade-input !w-auto !py-1" aria-label="Tag" value={tag} onChange={(e) => setTag(e.target.value)}>
            <option value="">All trades</option>
            {tags.map((t) => <option key={t} value={t}>{t}</option>)}
          </select>
        </label>}
        <Segmented label="Attempts" value={scope} onChange={setScope} options={[{ value: "current", label: "This attempt" }, { value: "all", label: "All attempts" }]} />
      </PageHeader>
      <div className="grid min-w-0 grid-cols-2 gap-3 md:grid-cols-3 xl:grid-cols-5">
        <Tile label="Net P&L (closed)" value={usd(stats.net)} tone={toneOf(stats.net)} detail={`fees ${formatMoney(stats.fees.toFixed(2))}`} />
        <Tile label="Win rate" value={stats.winRate == null ? "—" : `${(stats.winRate * 100).toFixed(1)}%`} detail={`${stats.wins} of ${stats.wins + stats.losses} decided`} />
        <Tile label="Profit factor" value={pf == null ? "—" : Number.isFinite(pf) ? pf.toFixed(2) : "∞"} detail="gross wins ÷ gross losses" />
        <Tile label="Average win" value={stats.averageWin == null ? "—" : usd(stats.averageWin)} tone="positive" />
        <Tile label="Average loss" value={stats.averageLoss == null ? "—" : usd(stats.averageLoss)} tone={stats.averageLoss == null ? "neutral" : "negative"} />
        <Tile label="Best trade" value={stats.best ? usd(tradeNet(stats.best)) : "—"} tone={stats.best ? toneOf(tradeNet(stats.best)) : "neutral"} detail={stats.best ? journalLabel(stats.best) : undefined} />
        <Tile label="Worst trade" value={stats.worst ? usd(tradeNet(stats.worst)) : "—"} tone={stats.worst ? toneOf(tradeNet(stats.worst)) : "neutral"} detail={stats.worst ? journalLabel(stats.worst) : undefined} />
        <Tile label="Trades" value={String(stats.trades)} detail={`${stats.contracts} contracts${stats.shares ? ` · ${stats.shares} shares` : ""}`} />
        <Tile label="Average hold" value={formatDuration(stats.averageHoldSeconds)} />
        <Tile label="Open trades" value={String(entries.filter((t) => t.status === "open").length)} />
      </div>
      <Calendar trades={entries} />
      <Reports trades={entries} />
      <History trades={list} trading={trading} />
      {!tag && shares.length > 0 && <Shares trades={shares} />}
    </div>
  )
}

function Calendar({ trades }: { trades: JournalTrade[] }) {
  const days = useMemo(() => dailyResults(trades), [trades])
  const live = useLive()
  const today = newYorkDate(new Date(marketNow(live)).toISOString())?.date ?? ""
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

function Reports({ trades }: { trades: JournalTrade[] }) {
  const [side, setSide] = useState<Side>("all")
  const [dimension, setDimension] = useState<Dimension>("duration")
  const buckets = tradeBuckets(trades, dimension, side)
  const by = dimension === "duration" ? "hold time" : dimension === "weekday" ? "weekday" : dimension === "month" ? "month" : "tag"
  return (
    <Panel title="Trade reports" actions={<>
      <Segmented label="Contracts" value={side} onChange={setSide} options={[{ value: "all", label: "All" }, { value: "call", label: "Calls" }, { value: "put", label: "Puts" }]} />
      <Segmented label="Group by" value={dimension} onChange={setDimension} options={[{ value: "duration", label: "Hold time" }, { value: "weekday", label: "Weekday" }, { value: "month", label: "Month" }, { value: "tag", label: "Tag" }]} />
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
const headers = ["Contract", "Side", "Qty", "Opened", "Closed", "Held", "Avg open", "Avg close", "Net P&L", "Return"]

function Tags({ trade }: { trade: Trade | undefined }) {
  return <>
    {trade?.tags?.map((tag) => <span key={tag} className="ml-1 rounded bg-raised px-1.5 py-0.5 text-[10px] font-normal text-muted">{tag}</span>)}
    {trade?.note && <span className="ml-1 text-[10px] text-accent" title={trade.note}>note</span>}
  </>
}

/** A trade's note and tags; a strategy's apply to each of its legs. */
function NoteEditor({ trades, trading }: { trades: Trade[]; trading: TradingStatus }) {
  const first = trades[0]!
  const token = useWriteToken()
  const refresh = useRefreshTrading()
  const [note, setNote] = useState(first.note ?? "")
  const [tags, setTags] = useState((first.tags ?? []).join(", "))
  const [pending, setPending] = useState(false)
  const [error, setError] = useState<unknown>()
  const [saved, setSaved] = useState(false)
  const dirty = note.trim() !== (first.note ?? "") || parseTags(tags).join() !== (first.tags ?? []).join()
  async function save() {
    setPending(true)
    setError(undefined)
    setSaved(false)
    try {
      for (const trade of trades) await api.annotateTrade(trade.id, { note: note.trim(), tags: parseTags(tags) }, trading.write)
      setSaved(true)
    } catch (failure) {
      setError(failure)
    } finally {
      setPending(false)
      void refresh()
    }
  }
  return <form className="max-w-xl space-y-2" onSubmit={(event) => { event.preventDefault(); void save() }}>
    <label className="trade-label">Note
      <textarea className="trade-input min-h-20" maxLength={2000} value={note} onChange={(e) => setNote(e.target.value)}
        placeholder="Why you took it, how you managed it, what you would change" /></label>
    <label className="trade-label">Tags
      <input className="trade-input" value={tags} onChange={(e) => setTags(e.target.value)} placeholder="breakout, 0dte, fomc" /></label>
    <WriteAccess trading={trading} />
    <TradingError error={error} />
    <div className="flex flex-wrap items-center gap-2">
      <button type="submit" className="trade-button" disabled={writeBlocked(trading, token) || pending || !dirty}>{pending ? "Saving…" : "Save note"}</button>
      {saved && !dirty && <span className="text-xs text-muted">Saved</span>}
      {trades.length > 1 && <span className="text-[11px] text-muted">Saved on each of its {trades.length} legs.</span>}
    </div>
  </form>
}

function TradeRow({ trade: t, expanded, onToggle }: { trade: Trade; expanded: boolean; onToggle: () => void }) {
  return <tr className="cursor-pointer border-t border-border/40 hover:bg-raised/50" onClick={onToggle} aria-expanded={expanded}>
    <td className="px-2 py-2 text-left">
      <span className={`mr-2 inline-block h-3 w-0.5 align-middle ${t.status === "open" ? "bg-accent" : tradeNet(t) >= 0 ? "bg-bullish" : "bg-bearish"}`} />
      <span className="font-medium">{contractLabel(t)}</span>
      {t.closure && <span className="ml-1 text-[10px] text-muted">({t.closure})</span>}
      <Tags trade={t} />
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
}

/** A strategy's round trips as one row: net P&L of its legs, opened at the order's net price. */
function StrategyRow({ group, expanded, onToggle }: { group: TradeGroup; expanded: boolean; onToggle: () => void }) {
  const order = group.order!
  // Open strategies count their closed legs' net and the open legs' unrealized P&L.
  const value = group.status === "open" ? (group.unrealised == null ? null : group.unrealised + group.net) : group.net
  const held = group.closed ? (Date.parse(group.closed) - Date.parse(group.opened)) / 1000 : null
  return <tr className="cursor-pointer border-t border-border/40 hover:bg-raised/50" onClick={onToggle} aria-expanded={expanded}>
    <td className="px-2 py-2 text-left">
      <span className={`mr-2 inline-block h-3 w-0.5 align-middle ${group.status === "open" ? "bg-accent" : group.net >= 0 ? "bg-bullish" : "bg-bearish"}`} />
      <span className="font-medium">{group.label}</span> <span className="text-[10px] text-muted">{group.trades.length} legs · #{order.id}</span>
      <Tags trade={group.trades[0]} />
    </td>
    <td className="px-2 py-2 text-accent">strategy</td>
    <td className="px-2 py-2">{order.filled_quantity}</td>
    <td className="px-2 py-2 text-muted">{short.format(Date.parse(group.opened))}</td>
    <td className="px-2 py-2 text-muted">{group.closed ? short.format(Date.parse(group.closed)) : "open"}</td>
    <td className="px-2 py-2">{formatDuration(held)}</td>
    <td className="px-2 py-2">{order.average_fill_price ? netLabel(order.average_fill_price) : "—"}</td>
    <td className="px-2 py-2">—</td>
    <td className={`px-2 py-2 ${toneText[toneOf(value)]}`}>
      {value == null ? "—" : group.status === "open" ? <span title="Closed legs' net plus open legs' unrealized">{usd(value)}</span> : usd(value)}
    </td>
    <td className="px-2 py-2">—</td>
  </tr>
}

function History({ trades, trading }: { trades: Trade[]; trading: TradingStatus }) {
  const [filter, setFilter] = useState<"closed" | "open" | "all">("closed")
  const [grouping, setGrouping] = useState<"trades" | "strategies">("trades")
  const [page, setPage] = useState(0)
  const [expanded, setExpanded] = useState<string | null>(null)
  const fills = useFills().data?.fills
  const orders = useAllOrders().data?.orders
  const fillsById = useMemo(() => new Map((fills ?? []).map((f) => [f.id, f])), [fills])
  const groups = useMemo(() => tradeGroups(trades, fills ?? [], orders ?? []), [trades, fills, orders])
  const items: TradeGroup[] = grouping === "strategies"
    ? groups.filter((g) => filter === "all" || g.status === filter)
    : trades.filter((t) => filter === "all" || t.status === filter).map((t) => ({ key: `trade-${t.id}`, order: null, label: "", trades: [t],
        status: t.status, opened: t.opened, closed: t.closed, net: tradeNet(t), unrealised: null }))
  const pages = Math.max(1, Math.ceil(items.length / pageSize))
  const current = Math.min(page, pages - 1)
  const visible = items.slice(current * pageSize, (current + 1) * pageSize)
  const net = trades.filter((t) => t.status === "closed").reduce((sum, t) => sum + tradeNet(t), 0)
  const toggle = (key: string) => setExpanded(expanded === key ? null : key)
  const detail = (t: Trade) => <TradeDetail trade={t} trading={trading} fills={t.fills.map((id) => fillsById.get(id)).filter((f): f is Fill => f != null)} />
  return (
    <Panel title="Trade history" actions={<>
      <span className="text-xs text-muted">Net <span className={`tabular ${toneText[toneOf(net)]}`}>{usd(net)}</span></span>
      <Segmented label="Group trades" value={grouping} onChange={(v) => { setGrouping(v); setPage(0); setExpanded(null) }}
        options={[{ value: "trades", label: "Contracts" }, { value: "strategies", label: "Strategies" }]} />
      <Segmented label="Trade status" value={filter} onChange={(v) => { setFilter(v); setPage(0) }}
        options={[{ value: "closed", label: "Closed" }, { value: "open", label: "Open" }, { value: "all", label: "All" }]} />
    </>}>
      {!items.length ? <p className="text-sm text-muted">No {filter === "all" ? "" : `${filter} `}trades yet.</p> : <>
        <div className="max-w-full overflow-x-auto" tabIndex={0} role="region" aria-label="Trade history">
          <table className="w-full text-right text-xs tabular whitespace-nowrap">
            <thead className="text-[11px] uppercase tracking-wide text-muted"><tr>
              {headers.map((h, i) => <th key={h} scope="col" className={`px-2 py-2 font-normal ${i === 0 ? "text-left" : ""}`}>{h}</th>)}
            </tr></thead>
            <tbody>
              {visible.map((group) => <Fragment key={group.key}>
                {group.order
                  ? <StrategyRow group={group} expanded={expanded === group.key} onToggle={() => toggle(group.key)} />
                  : <TradeRow trade={group.trades[0]!} expanded={expanded === group.key} onToggle={() => toggle(group.key)} />}
                {expanded === group.key && (group.order
                  ? <>
                    {group.trades.map((t) => <TradeRow key={t.id} trade={t} expanded={false} onToggle={() => {}} />)}
                    <tr className="bg-raised/30"><td colSpan={headers.length} className="px-4 py-3 text-left">
                      <NoteEditor key={group.trades.map((t) => t.id).join()} trades={group.trades} trading={trading} />
                    </td></tr>
                  </>
                  : <tr className="bg-raised/30"><td colSpan={headers.length} className="px-4 py-3 text-left">{detail(group.trades[0]!)}</td></tr>)}
              </Fragment>)}
            </tbody>
          </table>
        </div>
        {pages > 1 && <div className="mt-3 flex items-center justify-between text-xs text-muted">
          <span>Showing {current * pageSize + 1}–{Math.min(items.length, (current + 1) * pageSize)} of {items.length}</span>
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

const shareHeaders = ["Shares", "Side", "Opened", "From", "Closed", "By", "Held", "Avg open", "Avg close", "Dividends", "Net P&L", "Return"]

/** Shares from exercise and assignment, round trip by round trip: how each came and went. */
function Shares({ trades }: { trades: ShareTrade[] }) {
  const net = trades.filter((t) => t.status === "closed").reduce((sum, t) => sum + tradeNet(t), 0)
  return (
    <Panel title="Shares" actions={<span className="text-xs text-muted">Net <span className={`tabular ${toneText[toneOf(net)]}`}>{usd(net)}</span></span>}>
      <div className="max-w-full overflow-x-auto" tabIndex={0} role="region" aria-label="Share trades">
        <table className="w-full text-right text-xs tabular whitespace-nowrap">
          <thead className="text-[11px] uppercase tracking-wide text-muted"><tr>
            {shareHeaders.map((h, i) => <th key={h} scope="col" className={`px-2 py-2 font-normal ${i === 0 ? "text-left" : ""}`}>{h}</th>)}
          </tr></thead>
          <tbody>
            {trades.map((t) => <tr key={t.id} className="border-t border-border/40">
              <td className="px-2 py-2 text-left">
                <span className={`mr-2 inline-block h-3 w-0.5 align-middle ${t.status === "open" ? "bg-accent" : tradeNet(t) >= 0 ? "bg-bullish" : "bg-bearish"}`} />
                <span className="font-medium">{t.symbol} {t.max_shares}</span>
              </td>
              <td className={`px-2 py-2 ${t.direction === "long" ? "text-bullish" : "text-bearish"}`}>{t.direction}</td>
              <td className="px-2 py-2 text-muted">{short.format(Date.parse(t.opened))}</td>
              <td className="px-2 py-2 text-left">{shareSourceLabel(t.opened_by, t.option, t.symbol, "opened", t.direction)}</td>
              <td className="px-2 py-2 text-muted">{t.closed ? short.format(Date.parse(t.closed)) : "open"}</td>
              <td className="px-2 py-2 text-left">{t.closed ? shareSourceLabel(t.closed_by, t.closing_option, t.symbol, "closed", t.direction) : "—"}</td>
              <td className="px-2 py-2">{formatDuration(t.duration_seconds)}</td>
              <td className="px-2 py-2">{formatMoney(t.average_open)}</td>
              <td className="px-2 py-2">{formatMoney(t.average_close)}</td>
              <td className={`px-2 py-2 ${toneText[toneOf(t.dividends)]}`}>{t.dividends && Number(t.dividends) !== 0 ? signedMoney(t.dividends) : "—"}</td>
              <td className={`px-2 py-2 ${toneText[toneOf(t.status === "open" ? t.unrealised : t.net)]}`}>
                {t.status === "open" ? <span title="Unrealized">{signedMoney(t.unrealised)}</span> : signedMoney(t.net)}
              </td>
              <td className={`px-2 py-2 ${toneText[toneOf(t.return)]}`}>{signedPercent(t.return)}</td>
            </tr>)}
          </tbody>
        </table>
      </div>
      <p className="mt-2 text-[11px] text-muted">Shares come only from exercise and assignment and trade without fees. Net P&L includes dividends; they count in the totals, calendar and reports above.</p>
    </Panel>
  )
}

function TradeDetail({ trade, fills, trading }: { trade: Trade; fills: Fill[]; trading: TradingStatus }) {
  return <div className="space-y-4"><div className="grid gap-4 md:grid-cols-[16rem_1fr]">
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
  <NoteEditor key={trade.id} trades={[trade]} trading={trading} />
  </div>
}
