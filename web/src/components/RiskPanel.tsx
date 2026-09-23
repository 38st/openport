import type { Bucket, Risk } from "../api/trading-types"
import { fixed, isNum, pct } from "../lib/format"
import { formatMoney } from "../lib/trading"

function Utilisation({ label, value, limit }: { label: string; value: number | null; limit: number }) {
  return <div className="space-y-1">
    <div className="flex justify-between gap-2 text-[11px] text-muted"><span>{label} · {pct(value, 1)}</span><span>Limit {fixed(limit, 0)}</span></div>
    <div role="progressbar" aria-label={`${label} utilisation`} aria-valuemin={0} aria-valuemax={100}
      aria-valuenow={isNum(value) ? Math.min(100, Math.max(0, value * 100)) : undefined} aria-valuetext={isNum(value) ? `${pct(value, 1)} of limit` : "Unavailable"}
      className="h-1.5 overflow-hidden rounded-full bg-raised"><div className={isNum(value) && value >= 1 ? "h-full bg-danger" : "h-full bg-accent"} style={{ width: `${isNum(value) ? Math.min(100, Math.max(0, value * 100)) : 0}%` }} /></div>
  </div>
}
function RiskBucket({ label, bucket }: { label: string; bucket: Bucket }) {
  return <section className="min-w-0 space-y-3 rounded-md border border-border p-3">
    <h3 className="text-xs font-medium">{label}</h3>
    <dl className="grid grid-cols-2 gap-3 text-xs tabular">{([
      ["Dollar delta", bucket.dollar_delta], ["Dollar gamma / 1%", bucket.dollar_gamma_1pct], ["Vega / vol point", bucket.vega], ["Theta / day", bucket.theta],
    ] as const).map(([name, value]) => <div className="min-w-0" key={name}><dt className="text-muted">{name}</dt><dd className="mt-1 break-all">{fixed(value, 2)}</dd></div>)}</dl>
    <Utilisation label="Delta" value={bucket.delta_utilisation} limit={bucket.limits.dollar_delta} />
    <Utilisation label="Vega" value={bucket.vega_utilisation} limit={bucket.limits.vega} />
    <div className="space-y-1 text-[11px] text-muted"><p>Reachable including open orders</p><p className="tabular break-words">Δ [{fixed(bucket.reachable.delta_low, 2)}, {fixed(bucket.reachable.delta_high, 2)}]</p><p className="tabular break-words">Vega [{fixed(bucket.reachable.vega_low, 2)}, {fixed(bucket.reachable.vega_high, 2)}]</p></div>
  </section>
}
export function RiskPanel({ risk }: { risk: Risk }) {
  return <div className="space-y-3">
    {!risk.complete && <p className="text-sm text-warn">Risk valuation incomplete · some analytics are unavailable.</p>}
    <p className="text-xs text-muted">Daily loss {formatMoney(risk.daily_loss)} / {formatMoney(risk.limits.max_daily_loss)} · Limits revision {risk.limits_revision}</p>
    <div className="grid gap-3 sm:grid-cols-2 xl:grid-cols-3"><RiskBucket label="Aggregate" bucket={risk.aggregate} />{Object.entries(risk.underlyings).map(([symbol, bucket]) => <RiskBucket key={symbol} label={symbol} bucket={bucket} />)}</div>
  </div>
}
