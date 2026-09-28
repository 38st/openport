import { useQuery } from "@tanstack/react-query"
import { useRef, useState } from "react"
import { api } from "../api/client"
import type { BacktestDayInput, BacktestReport, BacktestResult, BacktestStart } from "../api/backtest-types"
import { useLive } from "../api/live"
import { BarChart } from "../charts/BarChart"
import { TradingError, WriteAccess, writeBlocked } from "../components/TradingControls"
import { Empty, Meter, PageHeader, Panel, Stat } from "../components/ui"
import { formatMoney, signedMoney } from "../lib/trading"
import { useWriteToken } from "../lib/write-token"

const label = "Simulated trading on recorded or generated days. Not a prediction. Not investment advice."
const percent = (value: number | null) => value == null ? "—" : `${(value * 100).toFixed(1)}%`
export function backtestHistogram(values: string[]) {
  const numbers = values.map(Number).filter(Number.isFinite)
  if (!numbers.length) return []
  const low = Math.min(...numbers), high = Math.max(...numbers)
  if (low === high) return [{ x: low, value: numbers.length }]
  const count = Math.min(10, numbers.length), width = (high - low) / count
  const bars = Array.from({ length: count }, (_, index) => ({ x: low + (index + .5) * width, value: 0 }))
  for (const number of numbers) bars[Math.min(count - 1, Math.floor((number - low) / width))]!.value++
  return bars
}
function Details({ result }: { result: BacktestResult }) {
  return <details className="text-xs"><summary>Trades, fills and rules</summary>
    <p className="my-2">Observed {result.started} to {result.ended}.</p>
    <p className="my-2">Journal: <code>{result.journal}</code></p>
    {!result.valuation_complete && <p className="text-warn">Incomplete marks. Last-mark P&amp;L: {signedMoney(result.last_mark_pnl)}.</p>}
    {result.open_positions > 0 && <p>{result.open_positions} positions remain open at the final observation.</p>}
    {result.quality_flags.length > 0 && <p className="text-warn">{result.quality_flags.join(", ")}</p>}
    {result.rule_trips.map((trip, index) => <p key={index}>{trip.time} · {trip.type} · {JSON.stringify(trip.detail)}</p>)}
    {Object.entries(result.entry_reasons).map(([key, reason]) => <p key={key}>{key}: {reason}</p>)}
    {result.trades.map((trade) => <p key={trade.order}>Order {trade.order} · {trade.closed ? signedMoney(trade.net) : "Open trade"} · {Object.entries(trade.rules).map(([rule, pass]) => `${rule}: ${pass == null ? "pending" : pass ? "pass" : "fail"}`).join(" · ")}</p>)}
    {result.stock_trades.map((trade, index) => <p key={`stock-${index}`}>{trade.symbol} shares · {trade.closed ? signedMoney(trade.net) : "Open share trade"}</p>)}
    {result.stock_fills.map((fill) => <p key={`stock-fill-${fill.id}`}>{fill.time} · {fill.shares} {fill.symbol} shares at {formatMoney(fill.price)}</p>)}
    {result.fills.map((fill) => <p key={fill.id}>{fill.time} · {fill.side} {fill.quantity} {fill.symbol} at {formatMoney(fill.price)} · fee {formatMoney(fill.fee)}</p>)}
  </details>
}
export function BacktestReportView({ report }: { report: BacktestReport }) {
  const summary = report.summary
  return <div className="space-y-4">
    <p className="text-sm text-muted">{report.label} {report.status !== "completed" && "Partial report; unfinished days and attempts are excluded."}</p>
    {report.errors.map((error, index) => <p role="alert" key={index}>{error.day == null ? "Attempt" : `Day ${error.day + 1}`}: {error.message}</p>)}
    <div className="grid gap-3 sm:grid-cols-4">
      <Stat label="Attempt pass rate" value={percent(summary.pass_rate)} hint="Passed / decided attempts. Open attempts are excluded." />
      <Stat label="Attempts" value={`${summary.passed} passed · ${summary.failed} failed · ${summary.open} open`} />
      <Stat label="Trade expectancy" value={signedMoney(summary.expectancy)} hint="Mean net result per closed strategy in independent daily runs." />
      <Stat label="Trade win rate" value={percent(summary.win_rate)} hint={`${summary.trades} closed strategies; breakeven trades are not wins.`} />
    </div>
    <p className="text-xs text-muted">{summary.completed_days} independent days, {summary.marked_days} with complete final marks. Day win rate: {percent(summary.day_win_rate)}. Attempts replay the ordered days on one account until a pass or failure, then start fresh on the next day.</p>
    <div className="grid gap-4 lg:grid-cols-2">{([
      ["Daily P&L distribution", summary.daily_pnl], ["Daily maximum drawdown distribution", summary.daily_drawdown],
    ] as const).map(([title, distribution]) => <Panel key={title} title={title}>
      <p className="text-xs text-muted">Min {formatMoney(distribution.min)} · Q1 {formatMoney(distribution.p25)} · Median {formatMoney(distribution.median)} · Q3 {formatMoney(distribution.p75)} · Max {formatMoney(distribution.max)}</p>
      <BarChart bars={backtestHistogram(distribution.values)} formatX={(value) => formatMoney(String(value), 0)} formatY={(value) => String(value)} height={220} />
      <p className="text-xs text-muted">Simulated results · horizontal: dollars; vertical: days</p>
    </Panel>)}</div>
    <Panel title="Worst days"><div className="flex flex-wrap gap-4 text-sm">{summary.worst_days.map((index) => {
      const day = report.days[index]
      return day && <button className="text-accent" key={index} onClick={() => document.getElementById(`backtest-day-${index}`)?.scrollIntoView({ block: "center" })}>{day.date} · {signedMoney(day.pnl)}</button>
    })}</div></Panel>
    <Panel title="Independent daily results"><div className="overflow-x-auto"><table className="w-full text-left text-xs tabular">
      <thead><tr>{["Day / source", "P&L", "Max drawdown", "Minimum floor room", "Adherence", "Fills / trips", "Review"].map((title) => <th className="p-2" key={title}>{title}</th>)}</tr></thead>
      <tbody>{report.days.map((day, index) => day && <tr id={`backtest-day-${index}`} className="border-t border-border" key={index}>
        <td className="p-2">{day.date}<div className="text-muted">{day.input.kind === "scenario" ? `Generated · ${day.input.id} · seed ${day.input.seed}` : `Recorded · ${day.input.name}`}</div></td>
        <td className="p-2">{signedMoney(day.pnl)}</td><td className="p-2">{formatMoney(day.max_drawdown)}</td><td className="p-2">{formatMoney(day.min_floor_distance)}</td>
        <td className="p-2">{percent(day.adherence)}</td><td className="p-2">{day.fills.length + day.stock_fills.length} / {day.rule_trips.length}</td><td className="p-2"><Details result={day} /></td>
      </tr>)}</tbody>
    </table></div></Panel>
    <Panel title="Evaluation attempts">{report.attempts.map((attempt, index) => <div className="space-y-2 border-b border-border py-2 text-sm" key={index}>
      <p>Attempt {index + 1} · {attempt.outcome} · days {attempt.first_day + 1}–{attempt.last_day + 1} · {signedMoney(attempt.pnl)}</p>
      {attempt.decision && <p className="text-xs text-muted">{attempt.decision}</p>}<Details result={attempt} />
    </div>)}</Panel>
  </div>
}
export function BacktestView() {
  const live = useLive(), token = useWriteToken()
  const definitions = useQuery({ queryKey: ["backtest-playbooks", live.accountScope], queryFn: ({ signal }) => api.backtestPlaybooks(signal) })
  const plans = useQuery({ queryKey: ["backtest-plans"], queryFn: ({ signal }) => api.backtestPlans(signal), staleTime: Infinity })
  const sources = useQuery({ queryKey: ["replay-listing"], queryFn: ({ signal }) => api.replay(signal) })
  const accountTrading = live.status?.trading ?? live.trading
  const trading = accountTrading && { ...accountTrading, write: sources.data?.write ?? accountTrading.write }
  const blocked = trading ? writeBlocked(trading, token) : "Paper trading is unavailable."
  const listing = useQuery({ queryKey: ["backtests"], queryFn: ({ signal }) => api.backtests(signal), refetchInterval: 1000 })
  const [selected, setSelected] = useState("")
  const activeId = selected || listing.data?.active || listing.data?.runs[0]?.id || ""
  const job = useQuery({ queryKey: ["backtest", activeId], queryFn: ({ signal }) => api.backtest(activeId, signal), enabled: !!activeId,
    refetchInterval: (query) => ["running", "cancelling"].includes(query.state.data?.status ?? "running") ? 1000 : false })
  const versions = Object.values(definitions.data?.definitions ?? {}).flatMap((record) => record.versions.map((definition) => ({ ...definition, archived: record.deleted })))
  const [playbook, setPlaybook] = useState(""), [plan, setPlan] = useState("eod-50k")
  const [kind, setKind] = useState("scenarios"), [count, setCount] = useState("10"), [seed, setSeed] = useState("1"), [scenario, setScenario] = useState("")
  const [files, setFiles] = useState<string[]>([]), [manifest, setManifest] = useState("[]")
  const [pending, setPending] = useState(false), [error, setError] = useState<unknown>()
  const busy = useRef(false)
  const chosen = playbook || (versions[0] ? `${versions[0].id}@${versions[0].version}` : "")
  const mode = trading?.write ?? "disabled"
  const badSeed = !/^\d{1,20}$/.test(seed) || BigInt(seed || "0") > 18446744073709551615n
  const badCount = !/^\d+$/.test(count) || Number(count) < 1 || Number(count) > 252
  async function mutate(action: () => Promise<void>) {
    if (busy.current) return
    busy.current = true; setPending(true); setError(undefined)
    try { await action(); await listing.refetch(); if (activeId) await job.refetch() }
    catch (failure) { setError(failure) } finally { busy.current = false; setPending(false) }
  }
  async function start() {
    let days: BacktestDayInput[] = []
    if (kind === "recordings") days = [...files].sort((left, right) => {
      const rows = sources.data?.recordings ?? []
      return (rows.find((row) => row.file === left)?.started ?? "").localeCompare(rows.find((row) => row.file === right)?.started ?? "") || left.localeCompare(right)
    }).map((file) => ({ file }))
    if (kind === "manifest") {
      const parsed: unknown = JSON.parse(manifest)
      if (!Array.isArray(parsed) || !parsed.length) throw new Error("The day manifest must be a nonempty JSON array.")
      if (parsed.some((entry) => entry && typeof entry === "object" && "seed" in entry && typeof entry.seed !== "string"))
        throw new Error("Write scenario seeds as quoted decimal strings to preserve all 64 bits.")
      days = parsed as BacktestDayInput[]
    }
    const request: BacktestStart = kind === "scenarios" ? { playbook: chosen, plan, scenarios: Number(count), seed, ...(scenario ? { scenario } : {}) }
      : { playbook: chosen, plan, days }
    const started = await api.startBacktest(request, mode)
    setSelected(started.id)
  }
  return <div className="space-y-4">
    <PageHeader title="Backtest" subtitle={label}>{trading && <WriteAccess trading={trading} />}</PageHeader>
    <TradingError error={error ?? listing.error ?? job.error ?? definitions.error ?? sources.error ?? plans.error} />
    <Panel title="Run a playbook">
      <div className="grid gap-3 sm:grid-cols-3">
        <label className="trade-label">Playbook version<select className="trade-input" value={chosen} onChange={(event) => setPlaybook(event.target.value)}>{versions.map((definition) => <option key={`${definition.id}@${definition.version}`} value={`${definition.id}@${definition.version}`}>{definition.name} · v{definition.version}{definition.archived ? " · archived" : ""}</option>)}</select></label>
        <label className="trade-label">Evaluation plan<select className="trade-input" value={plan} onChange={(event) => setPlan(event.target.value)}>{plans.data?.plans.filter((item) => item.rules.phase !== "funded" && item.rules.profit_target != null && Number(item.rules.profit_target) > 0).map((item) => <option key={item.id} value={item.id}>{item.name}</option>)}</select></label>
        <label className="trade-label">Days<select className="trade-input" value={kind} onChange={(event) => setKind(event.target.value)}><option value="scenarios">Generated scenarios</option><option value="recordings">Recordings, including imports</option><option value="manifest">Mixed day manifest</option></select></label>
      </div>
      {kind === "scenarios" && <div className="mt-3 grid gap-3 sm:grid-cols-3">
        <label className="trade-label">Scenario<select className="trade-input" value={scenario} onChange={(event) => setScenario(event.target.value)}><option value="">Cycle regular scenarios</option>{sources.data?.demos?.map((item) => <option key={item.id} value={item.id}>{item.title}</option>)}</select></label>
        <label className="trade-label">Day count<input className="trade-input" type="number" min="1" max="252" value={count} onChange={(event) => setCount(event.target.value)} /></label>
        <label className="trade-label">Starting seed<input className="trade-input" inputMode="numeric" value={seed} onChange={(event) => setSeed(event.target.value)} /></label>
      </div>}
      {kind === "recordings" && <div className="my-3 space-y-2">{sources.data?.recordings.map((recording) => <label className="flex gap-2 text-sm" key={recording.file}>
        <input type="checkbox" disabled={!!recording.error} checked={files.includes(recording.file)} onChange={(event) => setFiles(event.target.checked ? [...files, recording.file] : files.filter((file) => file !== recording.file))} />{recording.file}{recording.error && ` · ${recording.error}`}
      </label>)}{!sources.data?.recordings.length && <Empty>No recordings available.</Empty>}</div>}
      {kind === "manifest" && <label className="trade-label mt-3">Day manifest JSON<textarea className="trade-input min-h-32 font-mono" value={manifest} onChange={(event) => setManifest(event.target.value)} placeholder={'[{"scenario":"reversal","date":"2026-09-16","seed":"1"},{"file":"next-day.oprec"}]'} /></label>}
      <p className="my-3 text-xs text-muted">Choose distinct, increasing trading dates. Generated runs advance one trading day and one seed per entry. Missing condition inputs prevent entry. Maximum 252 days. Journals are retained for verification.</p>
      {blocked && <p className="mb-2 text-xs text-warn">{blocked}</p>}
      <button className="trade-button" disabled={!!blocked || pending || !!listing.data?.active || !chosen || (kind === "scenarios" && (badSeed || badCount)) || (kind === "recordings" && !files.length)} onClick={() => void mutate(start)}>Start backtest</button>
    </Panel>
    {!!listing.data?.runs.length && <label className="trade-label">Saved report<select className="trade-input" value={activeId} onChange={(event) => setSelected(event.target.value)}>{listing.data.runs.map((run) => <option key={run.id} value={run.id}>{run.id} · {run.status}</option>)}</select></label>}
    {job.data && <Panel title={`Run ${job.data.id} · ${job.data.status}`}>
      <p className="mb-2 text-sm" role="status">{job.data.phase} · {job.data.completed} / {job.data.total} day runs</p>
      <Meter value={job.data.total ? job.data.completed / job.data.total : 0} label="Backtest progress" />
      {job.data.directory && <p className="my-2 text-xs text-muted">Reports and journals: <code>{job.data.directory}</code></p>}
      {job.data.error && <p role="alert">{job.data.error}</p>}
      {["running", "cancelling"].includes(job.data.status) && <button className="trade-button mt-3" disabled={!!blocked || pending || job.data.status === "cancelling"} onClick={() => void mutate(async () => { await api.cancelBacktest(job.data!.id, mode) })}>Cancel backtest</button>}
    </Panel>}
    {job.data?.report && <BacktestReportView report={job.data.report} />}
  </div>
}
