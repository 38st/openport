import { useMemo, useState } from "react"
import { useLive } from "../api/live"
import { useAccount, usePlans, useTrades } from "../api/trading"
import type { Account, Trade, TradingStatus } from "../api/trading-types"
import { LineChart, type Reference, type Series } from "../charts/LineChart"
import { ResetDialog, planFacts } from "../components/ResetDialog"
import { evaluationBadge } from "../components/Sidebar"
import { TradingError } from "../components/TradingControls"
import { Check, Empty, PageHeader, Panel, Tile, toneOf, toneText } from "../components/ui"
import { money, signedPercent } from "../lib/format"
import { timestampET } from "../lib/freshness"
import { contractLabel, formatDuration } from "../lib/journal"
import { offeredPlans, unlockedFundedPlan } from "../lib/payouts"
import { useRoute } from "../lib/route"
import { formatMoney, ratio, signedMoney, subtractMoney } from "../lib/trading"

const dayFormat = new Intl.DateTimeFormat("en-US", { month: "short", day: "numeric", timeZone: "America/New_York" })

/** Starting balance, each finished day's close and the live mark, with the floor as it trailed. */
export function equitySeries(account: Account): { series: Series[]; references: Reference[]; ticks: number[] } {
  const e = account.evaluation
  const start = Date.parse(e.started)
  const now = Math.max(Date.parse(account.time), start + 1)
  const startFloor = account.rules.max_drawdown != null ? Number(e.starting_balance) - Number(account.rules.max_drawdown) : null
  const equity = [{ x: start, y: Number(e.starting_balance) }]
  const floor = startFloor == null ? [] : [{ x: start, y: startFloor }]
  for (const day of e.days) {
    // Plot each finished day at its 16:00 New York close (20:00 UTC during daylight time).
    const x = Math.min(Math.max(Date.parse(`${day.day}T20:00:00Z`), (equity[equity.length - 1]?.x ?? start) + 1), now - 1)
    equity.push({ x, y: Number(day.close_equity) })
    if (startFloor != null && day.floor != null) floor.push({ x, y: Number(day.floor) })
  }
  equity.push({ x: now, y: Number(e.equity) })
  if (startFloor != null && e.floor != null) floor.push({ x: now, y: Number(e.floor) })
  const series: Series[] = [{ id: "equity", label: "Equity", color: "var(--chart-1)", points: equity, area: true }]
  if (floor.length) series.push({ id: "floor", label: "Drawdown floor", color: "var(--bearish)", points: floor, dashed: true })
  const references: Reference[] = e.target_equity != null ? [{ y: Number(e.target_equity), label: "Target", color: "var(--bullish)" }] : []
  // One tick per New York date, thinned to at most eight.
  const seen = new Set<string>()
  const days = equity.filter((p) => { const key = dayFormat.format(p.x); return !seen.has(key) && seen.add(key) }).map((p) => p.x)
  const every = Math.ceil(days.length / 8)
  return { series, references, ticks: days.filter((_, i) => i % every === 0) }
}

export function DashboardView() {
  const { trading, accountScope } = useLive()
  if (!trading) return null
  return <Dashboard key={accountScope} trading={trading} />
}

function Dashboard({ trading }: { trading: TradingStatus }) {
  const account = useAccount()
  const trades = useTrades("current")
  const { underlyings } = useLive()
  const plans = usePlans()
  const [, navigate] = useRoute()
  // null: closed; "": open without a preselected plan.
  const [resetting, setResetting] = useState<string | null>(null)
  const data = account.data
  const chart = useMemo(() => (data ? equitySeries(data) : null), [data])
  if (account.error) return <TradingError error={account.error} />
  if (!data || !chart) return <Empty>{trading.enabled ? "Loading account…" : trading.reason ?? "Paper trading is unavailable"}</Empty>
  const e = data.evaluation
  const r = data.rules
  const profit = e.profit
  const target = r.profit_target
  const targetProgress = target ? ratio(profit, target) : null
  const floorRange = e.floor != null ? subtractMoney(e.peak, e.floor) : null
  const bufferShare = e.drawdown_buffer != null ? ratio(e.drawdown_buffer, floorRange) : null
  const today = subtractMoney(e.equity, e.day_open_equity)
  const recent = (trades.data?.trades ?? []).filter((t) => t.status === "closed").slice(0, 5)
  const funded = r.phase === "funded"
  const payout = data.payout
  const unlocked = unlockedFundedPlan(offeredPlans(plans.data?.plans ?? []), data)

  return (
    <div className="min-w-0 space-y-4">
      <PageHeader
        title={<span className="flex flex-wrap items-center gap-2">Dashboard {evaluationBadge(data, r.plan)}</span>}
        subtitle={<>{r.plan ?? "Paper account"} · attempt {e.attempt} · started {timestampET(e.started)}</>}>
        <button type="button" className="trade-button" onClick={() => setResetting("")} disabled={!trading.enabled}>
          {e.enabled ? "New attempt" : "Start an evaluation"}
        </button>
      </PageHeader>

      {e.status !== "active" && (
        <div role="status" className={`rounded-lg border p-4 ${e.status === "passed" ? "border-bullish/50 bg-bullish/5" : "border-bearish/50 bg-bearish/5"}`}>
          <div className={`font-medium ${e.status === "passed" ? "text-bullish" : "text-bearish"}`}>
            {e.status === "passed" ? "Evaluation passed" : funded ? "Funded account closed" : "Evaluation failed"}
          </div>
          <p className="mt-1 text-sm">{e.decision}</p>
          <p className="mt-1 text-xs text-muted">Decided {timestampET(e.decided_at)}. Positions are closed and new orders are refused until you start a new attempt.</p>
          {unlocked && <p className="mt-2 text-sm">Your <strong>{unlocked.name}</strong> account is unlocked: the same size and drawdown, no profit target, and payouts from your profits.</p>}
          <div className="mt-3 flex flex-wrap gap-2">
            {unlocked && <button type="button" className="trade-button border-bullish/60" onClick={() => setResetting(unlocked.id)} disabled={!trading.enabled}>Start {unlocked.name}</button>}
            <button type="button" className="trade-button" onClick={() => setResetting("")} disabled={!trading.enabled}>Start a new attempt</button>
          </div>
        </div>
      )}
      {!e.enabled && (
        <div className="rounded-lg border border-border bg-panel p-4 text-sm">
          <div className="font-medium">No evaluation running</div>
          <p className="mt-1 text-muted">This account has no profit target or drawdown floor. Start an evaluation to trade against a target
            and a trailing drawdown, with results tracked below.</p>
        </div>
      )}
      {!e.marked && <p role="status" className="text-xs text-warn">Some positions have no mark yet; rules wait for fully marked equity.</p>}

      <div className="grid min-w-0 gap-3 xl:grid-cols-[minmax(0,1fr)_18rem]">
        <Panel title="Equity" actions={<span className="text-[11px] text-muted">{e.days.length ? `${e.days.length} finished day${e.days.length === 1 ? "" : "s"}` : "first day"}</span>}>
          <LineChart series={chart.series} references={chart.references} xTicks={chart.ticks} height={300} marginLeft={64}
            formatX={(x) => dayFormat.format(x)} formatY={(y) => money(y)} />
          <div className="mt-2 flex flex-wrap gap-4 text-[11px] text-muted">
            <span className="flex items-center gap-1.5"><span className="h-0.5 w-4 bg-[var(--chart-1)]" />Equity</span>
            {e.target_equity != null && <span className="flex items-center gap-1.5"><span className="h-0 w-4 border-t border-dashed border-bullish" />Profit target</span>}
            {e.floor != null && <span className="flex items-center gap-1.5"><span className="h-0 w-4 border-t border-dashed border-bearish" />Drawdown floor ({e.floor_locked ? "locked" : r.drawdown_mode === "intraday" ? "trails intraday" : "trails at the close"})</span>}
          </div>
        </Panel>
        <div className="grid min-w-0 grid-cols-2 gap-3 xl:grid-cols-1">
          <Tile label="Net P&L" value={signedMoney(profit)} tone={toneOf(profit)}
            detail={`${signedPercent(ratio(profit, e.starting_balance), 2)} of starting balance`} />
          {payout ? <Tile label="Next payout" value={payout.eligible ? formatMoney(payout.maximum) : `${payout.qualifying_days} / ${payout.required_days} days`}
            detail={payout.eligible ? `Available now · ${formatMoney(payout.trader_share)} to you` : `Qualifying days · ${formatMoney(payout.qualifying_profit, 0)}+ net each`}
            meter={{ value: payout.qualifying_days / payout.required_days, tone: "positive", label: "Qualifying days toward the next payout" }} />
          : <Tile label="Profit target" value={target ? formatMoney(e.target_equity) : "None"}
            detail={target ? `${formatMoney(e.target_remaining)} to go · ${Math.max(0, (targetProgress ?? 0) * 100).toFixed(1)}%` : "Practice has no target"}
            meter={target ? { value: targetProgress, tone: "positive", label: "Progress to profit target" } : undefined} />}
          <Tile label="Equity" value={formatMoney(e.equity)} detail={`Peak ${formatMoney(e.peak)} · today ${signedMoney(today)}`} />
          <Tile label="Drawdown floor" value={e.floor != null ? formatMoney(e.floor) : "None"}
            tone={bufferShare != null && bufferShare < 0.25 ? "negative" : "neutral"}
            detail={e.floor != null ? `${formatMoney(e.drawdown_buffer)} buffer${e.floor_locked ? " · locked" : ""}` : "Practice has no floor"}
            meter={bufferShare != null ? { value: bufferShare, tone: bufferShare < 0.25 ? "negative" : bufferShare < 0.5 ? "warn" : "positive", label: "Buffer above the drawdown floor" } : undefined} />
        </div>
      </div>

      <dl className="grid grid-cols-2 gap-px overflow-hidden rounded-lg border border-border bg-border text-xs sm:grid-cols-3 xl:grid-cols-6">
        {([
          ["Started", timestampET(e.started)],
          ["Plan", r.plan ?? "Paper account"],
          ["Starting balance", formatMoney(e.starting_balance)],
          ["Drawdown", r.max_drawdown ? `${formatMoney(r.max_drawdown, 0)} · ${r.drawdown_mode === "intraday" ? "intraday" : "end of day"}` : "None"],
          ["Buying power", formatMoney(data.buying_power.available)],
          ["Updated", timestampET(data.time)],
        ] as const).map(([label, value]) => (
          <div key={label} className="min-w-0 bg-panel px-3 py-2">
            <dt className="text-[10px] uppercase tracking-wide text-muted">{label}</dt>
            <dd className="mt-0.5 truncate tabular" title={value}>{value}</dd>
          </div>
        ))}
      </dl>

      <Panel title="How am I doing?">
        <div className="grid gap-6 md:grid-cols-2">
          <div>
            <h3 className="mb-2 text-[11px] font-medium uppercase tracking-wide text-accent">Progress</h3>
            <ul className="space-y-1.5 text-sm">
              <Check ok={toneOf(profit) !== "negative"} label="Current P&L" value={signedMoney(profit)} tone={toneOf(profit)} />
              {target && <Check ok={e.status === "passed"} label="Remaining to target" value={formatMoney(e.target_remaining)} />}
              {payout && <Check ok={payout.qualifying_days >= payout.required_days} label="Qualifying days this cycle" value={`${payout.qualifying_days} of ${payout.required_days}`} />}
              {payout && <Check ok={payout.eligible} label={payout.eligible ? "Payout available now" : "Next payout, once eligible"} value={`up to ${formatMoney(payout.maximum)}`} />}
              {e.floor != null && <Check ok={e.status !== "failed"} label="Drawdown left" value={formatMoney(e.drawdown_buffer)} />}
              <Check ok label="Starting balance" value={formatMoney(e.starting_balance)} />
              <Check ok label="Peak equity" value={formatMoney(e.peak)} />
              <Check ok={toneOf(today) !== "negative"} label="Today" value={signedMoney(today)} tone={toneOf(today)} />
            </ul>
          </div>
          <div>
            <h3 className="mb-2 text-[11px] font-medium uppercase tracking-wide text-accent">Rules</h3>
            <ul className="space-y-1.5 text-sm">
              {planFacts({ initial_cash: e.starting_balance, rules: r }).slice(1).map((fact) => <Check key={fact} ok label={fact} />)}
              <Check ok label="Regular trading hours only" />
              <Check ok label={`Underlyings: ${underlyings.map((u) => u.symbol).join(", ") || "none configured"}`} />
            </ul>
          </div>
        </div>
      </Panel>

      <div className="grid gap-3 lg:grid-cols-2">
        <Panel title="Recent trades" actions={<button type="button" onClick={() => navigate({ view: "journal" })} className="text-xs text-accent hover:underline">Open journal</button>}>
          {recent.length ? <RecentTrades trades={recent} /> : <p className="text-sm text-muted">No closed trades in this attempt yet.</p>}
        </Panel>
        <Panel title="Attempts">
          {data.attempts.length ? (
            <ul className="space-y-2 text-sm">
              {[...data.attempts].reverse().map((a) => (
                <li key={a.attempt} className="flex flex-wrap items-center justify-between gap-2 border-b border-border/40 pb-2 last:border-0">
                  <span>#{a.attempt} · {a.plan ?? "Paper account"} <span className="text-xs text-muted">{timestampET(a.started)}</span></span>
                  <span className="flex items-center gap-2 tabular">
                    <span className={toneText[toneOf(subtractMoney(a.final_equity, a.starting_balance))]}>{signedMoney(subtractMoney(a.final_equity, a.starting_balance))}</span>
                    <span className="text-xs text-muted">{a.status}</span>
                  </span>
                </li>
              ))}
            </ul>
          ) : <p className="text-sm text-muted">This is the first attempt on this account.</p>}
        </Panel>
      </div>
      {resetting != null && <ResetDialog trading={trading} attempt={e.attempt} initial={resetting || undefined} onClose={() => setResetting(null)} />}
    </div>
  )
}

function RecentTrades({ trades }: { trades: Trade[] }) {
  return (
    <ul className="space-y-2 text-sm">
      {trades.map((t) => (
        <li key={t.id} className="flex flex-wrap items-center justify-between gap-2">
          <span className="min-w-0">
            <span className="font-medium">{contractLabel(t)}</span>{" "}
            <span className="text-xs text-muted">{t.direction} {t.max_quantity} · held {formatDuration(t.duration_seconds)}</span>
          </span>
          <span className={`tabular ${toneText[toneOf(t.net)]}`}>{signedMoney(t.net)}</span>
        </li>
      ))}
    </ul>
  )
}
