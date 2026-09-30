import type { Breach, BreachLevel } from "../api/trading-types"
import { formatMoney } from "../lib/trading"

function level(value: BreachLevel | null) {
  if (!value) return "No crossing in scan"
  return `${value.points.toFixed(2)} points (${value.percent.toFixed(2)}%) · ${value.touch_probability == null ? "probability unavailable" : `${(value.touch_probability * 100).toFixed(1)}% model touch estimate`}`
}
export function BreachPanel({ breach }: { breach?: Breach }) {
  if (!breach) return null
  // Levels reach the plan floor, or the soft floor on an account without one.
  const target = breach.room != null ? "floor" : breach.soft_room != null ? "soft floor" : null
  return <section aria-label="Breach risk" className="space-y-2 text-xs">
    <div className="flex flex-wrap gap-x-5 gap-y-1 tabular">
      <span>Room above floor <strong>{formatMoney(breach.room)}</strong></span>
      {breach.soft_room != null && <span>Room above soft floor <strong>{formatMoney(breach.soft_room)}</strong></span>}
    </div>
    {!breach.complete && <p className="text-warn">Breach risk unavailable for positions without current marks or valuations.</p>}
    {!target && !!breach.underlyings.length && <p className="text-muted">No plan or soft floor, so there are no breach levels to show.</p>}
    {target && breach.underlyings.map((item) => <div key={item.underlying} className="rounded-md border border-border p-2">
      <strong>{item.underlying}</strong>{item.complete ? <dl className="mt-1 space-y-1 tabular">
        <div><dt className="inline text-muted">Down to {target}: </dt><dd className="inline">{level(item.down)}</dd></div>
        <div><dt className="inline text-muted">Up to {target}: </dt><dd className="inline">{level(item.up)}</dd></div>
      </dl> : <p className="text-muted">Scenario inputs unavailable.</p>}
    </div>)}
    {target && !!breach.underlyings.length && <p className="text-faint">Model estimates to today's close at unchanged volatility, one underlying moving at a time. Search: −99.75% to +1000%. These are simulated scenarios, not forecasts.</p>}
  </section>
}
