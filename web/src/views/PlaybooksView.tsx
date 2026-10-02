import { useQueryClient } from "@tanstack/react-query"
import { useState } from "react"
import { api } from "../api/client"
import { useLive } from "../api/live"
import type { Playbook, PlaybookStats } from "../api/playbook-types"
import { PassOddsCard, StagedOrders, usePlaybooks } from "../components/Playbooks"
import { TradingError, WriteAccess, writeBlocked } from "../components/TradingControls"
import { Empty, PageHeader, Panel } from "../components/ui"
import { formatMoney } from "../lib/trading"
import { useWriteToken } from "../lib/write-token"

export const newPlaybook: Playbook = {
  id: "put-spread", name: "Morning put spread", description: "Write the setup and its invalidation here.", underlyings: ["SPX"],
  window: { start: "09:45", end: "11:00", weekdays: [1, 2, 3, 4, 5] }, conditions: {},
  structure: { template: { kind: "vertical", type: "put", direction: "credit", target: { mode: "delta", value: 15 }, width: 5 }, expiry: { min: 0, max: 1 } },
  sizing: { units: 1 }, management: { take_profit_percent: 50, stop_credit_multiple: 2, close_by: "15:45" },
  guardrails: { max_entries_per_day: 1, cooldown_minutes: 30 },
}
const pct = (value: number | null) => value == null ? "—" : `${(100 * value).toFixed(1)}%`
function Stats({ rows }: { rows: [string, PlaybookStats][] }) {
  return <div className="overflow-x-auto"><table className="w-full text-left text-xs tabular">
    <thead className="text-muted"><tr>{["Trades", "Count", "Win rate", "Average win", "Average loss", "Expectancy", "Profit factor", "Average R", "Return on BP", "Adherence"].map((label) => <th className="p-2 font-normal" key={label}>{label}</th>)}</tr></thead>
    <tbody>{rows.map(([label, row]) => <tr key={label} className="border-t border-border">
      <th className="p-2 font-normal">{label}</th><td>{row.trades}</td><td>{pct(row.win_rate)}</td><td>{formatMoney(row.average_win)}</td><td>{formatMoney(row.average_loss)}</td>
      <td>{formatMoney(row.expectancy)}</td><td>{row.no_losses ? "∞" : row.profit_factor?.toFixed(2) ?? "—"}</td><td>{row.average_r?.toFixed(2) ?? "—"}</td><td>{pct(row.average_return_on_buying_power ?? null)}</td><td>{pct(row.adherence)}</td>
    </tr>)}</tbody>
  </table></div>
}
export function PlaybooksView() {
  const live = useLive()
  return <Playbooks key={live.accountScope} />
}
function Playbooks() {
  const live = useLive()
  const query = usePlaybooks()
  const client = useQueryClient()
  const token = useWriteToken()
  const [selected, setSelected] = useState("")
  const [editing, setEditing] = useState<string | null>(null)
  const [error, setError] = useState<unknown>(null)
  const [busy, setBusy] = useState(false)
  if (!live.trading) return null
  const trading = live.trading
  const blocked = writeBlocked(trading, token)
  const mutate = async (work: () => Promise<unknown>, close = false) => {
    setBusy(true); setError(null)
    try { await work(); if (close) setEditing(null); await client.invalidateQueries({ queryKey: ["playbooks"] }) }
    catch (failure) { setError(failure) } finally { setBusy(false) }
  }
  if (query.error) return <TradingError error={query.error} />
  if (!query.data) return <Empty>{trading.enabled ? "Loading playbooks…" : trading.reason ?? "Paper trading is unavailable"}</Empty>
  const data = query.data
  const definitions = Object.values(data.definitions).filter((item) => !item.deleted).map((item) => item.versions.at(-1)!)
  const archived = Object.values(data.definitions).filter((item) => item.deleted).map((item) => item.versions.at(-1)!)
  const current = data.definitions[selected]?.versions.at(-1)
  const report = data.reports?.[selected]
  return <div className="space-y-4">
    <PageHeader title="Playbooks" subtitle="Written setups, staged orders and adherence for this account.">
      <button className="trade-button" disabled={!!blocked || busy} onClick={() => { setEditing(JSON.stringify(newPlaybook, null, 2)); setError(null) }}>New playbook</button>
    </PageHeader>
    <WriteAccess trading={trading} />
    <p className="text-xs text-muted">Orders and fills are simulated. Auto is available only in replay and scenario accounts. All times are New York time.</p>
    {error != null && <TradingError error={error} />}
    <div className="grid gap-3 lg:grid-cols-2">{definitions.map((definition) => <Panel key={definition.id} title={`${definition.name} · v${definition.version}`}>
      <p className="text-sm">{definition.description}</p><p className="mt-2 text-xs text-muted">{definition.underlyings.join(", ")} · {definition.window.start}–{definition.window.end} ET · {definition.structure.template.kind}</p>
      <div className="mt-3 flex flex-wrap items-center gap-2">
        <label className="text-xs">Mode <select className="trade-input !w-auto" aria-label={`Mode for ${definition.name}`} value={data.modes[definition.id] ?? "off"} disabled={!!blocked || busy}
          onChange={(event) => void mutate(() => api.playbookMode(definition.id, event.target.value as "off" | "stage" | "auto", trading.write))}>
          <option value="off">Off</option><option value="stage">Stage</option><option value="auto" disabled={!data.auto_allowed}>Auto (replay only)</option>
        </select></label>
        <button className="trade-button" onClick={() => setSelected(definition.id)}>Stats and versions</button>
        <button className="trade-button" disabled={!!blocked || busy} onClick={() => { setEditing(JSON.stringify(definition, null, 2)); setError(null) }}>Edit {definition.name}</button>
        <button className="trade-button" disabled={!!blocked || busy} onClick={() => void mutate(() => api.deletePlaybook(definition.id, definition.version!, trading.write))}>Archive {definition.name}</button>
      </div>
      {Object.entries(data.reasons).filter(([key]) => key.startsWith(`${definition.id}:`)).map(([key, reason]) => <p key={key} className="mt-2 text-xs text-muted">{key.split(":")[1]}: {reason}</p>)}
    </Panel>)}</div>
    {!definitions.length && <Empty>No playbooks yet. Create a setup to stage it on this account.</Empty>}
    {!!archived.length && <details className="text-xs"><summary>Archived playbooks</summary>
      <div className="mt-2 flex flex-wrap gap-2">{archived.map((definition) => <button className="trade-button" key={definition.id} onClick={() => setSelected(definition.id)}>{definition.name} · archived stats and versions</button>)}</div>
    </details>}
    {editing != null && <Panel title="Edit playbook definition">
      <p className="mb-3 text-xs text-muted">This JSON contains the complete rules. Saving an edit creates a new version; old versions remain readable. Use fixed units or floor_share (0–1). Take-profit percent is the closing premium as a percent of the entry credit or debit. Price references use point offsets; IV rank uses fractions. Missing condition inputs prevent entry.</p>
      <label className="trade-label">Definition JSON<textarea aria-label="Playbook definition" className="trade-input min-h-96 font-mono text-xs" value={editing} onChange={(event) => setEditing(event.target.value)} /></label>
      <div className="mt-3 flex gap-2"><button className="trade-button" disabled={!!blocked || busy} onClick={() => void mutate(() => api.savePlaybook(JSON.parse(editing) as Playbook, trading.write), true)}>Save version</button>
        <button className="trade-button" onClick={() => setEditing(null)}>Cancel edit</button></div>
    </Panel>}
    <StagedOrders />
    {current && <>
      <Panel title={`${current.name} · this attempt`}>
        {report && <Stats rows={[["All", report.all], ["Followed rules", report.followed], ["Deviated", report.deviated]]} />}
        <p className="my-3 text-xs text-muted">Unfinished time-stop rules are pending. Missing sizing evidence fails the size check. Trades count once per opening strategy.</p>
        {report?.trades.map((trade) => <details key={trade.order} className="border-t border-border py-2 text-xs">
          <summary>Order {trade.order} · v{trade.version} · {trade.closed ? formatMoney(trade.net) : "Open"} · {trade.followed ? "Rules followed so far" : "Deviation"}</summary>
          <dl className="mt-2 space-y-1">{Object.entries(trade.rules).map(([rule, pass]) => <div className="flex gap-3" key={rule}><dt>{rule.replaceAll("_", " ")}</dt><dd>{pass == null ? "Pending" : pass ? "Pass" : "Fail"}</dd></div>)}</dl>
        </details>)}
        <details className="mt-3 text-xs"><summary>Saved versions</summary>{data.definitions[selected]?.versions.map((definition) => <details key={definition.version} className="mt-2"><summary>Version {definition.version}</summary><pre className="overflow-x-auto p-2">{JSON.stringify(definition, null, 2)}</pre></details>)}</details>
      </Panel>
      <PassOddsCard playbook={selected} />
    </>}
  </div>
}
