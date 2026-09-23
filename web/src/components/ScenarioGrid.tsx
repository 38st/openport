import type { Scenarios } from "../api/trading-types"
import { formatMoney, scenarioColour, scenarioScale } from "../lib/trading"

export function ScenarioGrid({ scenarios }: { scenarios: Scenarios }) {
  const scale = scenarioScale(scenarios.pnl)
  return <div className="space-y-2">
    <p className="text-xs text-muted">Spot change (%) × volatility change (points) · P&amp;L in dollars</p>
    {!scenarios.complete && <p className="text-xs text-warn">Scenario valuation incomplete; unavailable cells are shown as —.</p>}
    <div className="max-w-full overflow-x-auto rounded-md border border-border" tabIndex={0} role="region" aria-label="Spot and volatility scenario grid">
      <table className="w-full border-collapse text-xs tabular">
        <thead><tr className="bg-raised text-muted"><th scope="col" className="p-2 text-left font-normal whitespace-nowrap">Spot / Vol</th>{scenarios.vol_points.map((vol, index) => <th scope="col" className="p-2 font-normal whitespace-nowrap" key={index}>{vol > 0 ? "+" : ""}{vol} vp</th>)}</tr></thead>
        <tbody>{scenarios.spot_percent.map((spot, row) => <tr key={row}>
          <th scope="row" className="bg-raised p-2 text-left font-normal whitespace-nowrap">{spot > 0 ? "+" : ""}{spot}%</th>
          {scenarios.vol_points.map((vol, column) => {
            const value = scenarios.pnl[row]?.[column] ?? null
            const clamped = scenarios.clamped[row]?.[column] ?? false
            const baseline = spot === 0 && vol === 0
            const display = value != null && Number.isFinite(value) ? formatMoney(String(value)) : "—"
            return <td key={column} style={{ background: scenarioColour(value, scale) }}
              title={`${spot}% spot, ${vol} vol points: ${value ?? "unavailable"}${clamped ? " · volatility clamped" : ""}${baseline ? " · unchanged spot and vol" : ""}`}
              className={`border border-border/50 p-2 text-right whitespace-nowrap ${baseline ? "font-semibold outline outline-2 -outline-offset-2 outline-accent" : ""}`}>{display}{clamped ? "*" : ""}</td>
          })}
        </tr>)}</tbody>
      </table>
    </div>
    <div className="flex flex-wrap justify-between gap-2 text-[11px] text-muted"><span><span className="text-bearish">Loss</span> ← 0 → <span className="text-bullish">Gain</span> · symmetric colour scale ±{formatMoney(String(scale))}</span><span>Outlined: unchanged spot and vol · * vol clamped</span></div>
  </div>
}
