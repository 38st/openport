import { useQuery } from "@tanstack/react-query"
import { useMemo, useState } from "react"
import { api } from "../api/client"
import { useLive } from "../api/live"
import { BarChart } from "../charts/BarChart"
import { Heatmap } from "../charts/Heatmap"
import { Empty, Panel, Segmented, Stat } from "../components/ui"
import { expiryLabel, fixed, isNum, money, price } from "../lib/format"
import { matchingPayload } from "../lib/payload"

type Metric = "gex" | "vex"

export function ExposureView({ symbol }: { symbol: string }) {
  const version = useLive().version(symbol)
  const [expiries, setExpiries] = useState(8)
  const [window, setWindow] = useState(0.05)
  const [metric, setMetric] = useState<Metric>("gex")

  const exposure = useQuery({
    queryKey: ["exposure", symbol, expiries, window, version],
    queryFn: ({ signal }) => api.exposure(symbol, expiries, window, signal),
    placeholderData: (previous) => matchingPayload(previous, symbol),
  })
  const data = matchingPayload(exposure.data, symbol)

  const totals = useMemo(() => {
    if (!data) return []
    return data.strikes.map((strike, i) => ({
      x: strike,
      value: data.expiries.reduce((sum, e) => sum + ((metric === "gex" ? e.gex : e.vex)[i] ?? 0), 0),
    }))
  }, [data, metric])

  const nearestStrike = useMemo(() => {
    if (!data || !isNum(data.spot)) return null
    const spot = data.spot
    return data.strikes.reduce<number | null>((best, s) => (best == null || Math.abs(s - spot) < Math.abs(best - spot) ? s : best), null)
  }, [data])

  if (exposure.isError) return <Empty>{String(exposure.error)}</Empty>
  if (!data) return <Empty>Loading exposure…</Empty>

  const summary = data.exposure
  const unit = metric === "gex" ? "per 1% move" : "per vol point"
  const markers = [
    ...(isNum(data.spot) ? [{ x: data.spot, label: `spot ${price(data.spot)}`, color: "var(--warn)" }] : []),
    ...(metric === "gex" && isNum(summary.gamma_flip) ? [{ x: summary.gamma_flip, label: "flip", color: "var(--muted)" }] : []),
  ]

  return (
    <div className="flex flex-col gap-3">
      <div className="grid grid-cols-2 gap-2 sm:grid-cols-3 lg:grid-cols-5">
        <Stat label="Net GEX" value={money(summary.gex)} tone={isNum(summary.gex) && summary.gex < 0 ? "negative" : "positive"} hint="Dollars of dealer hedging per 1% move in spot, all expiries" />
        <Stat label="Net VEX" value={money(summary.vex)} tone={isNum(summary.vex) && summary.vex < 0 ? "negative" : "positive"} hint="Dollars of delta per vol point, all expiries" />
        <Stat label="Gamma flip" value={price(summary.gamma_flip)} tone="warn" hint="Spot level where net GEX changes sign (sticky-strike vols)" />
        <Stat label="Call wall" value={fixed(summary.call_wall, 0)} tone="positive" hint="Strike with the largest positive GEX" />
        <Stat label="Put wall" value={fixed(summary.put_wall, 0)} tone="negative" hint="Strike with the most negative GEX" />
      </div>

      <Panel
        title={`${metric === "gex" ? "Gamma" : "Vanna"} exposure by strike · ${unit}`}
        actions={
          <>
            <Segmented
              label="Metric"
              value={metric}
              options={[
                { value: "gex", label: "GEX" },
                { value: "vex", label: "VEX" },
              ]}
              onChange={setMetric}
            />
            <Segmented label="Expiries" value={expiries} options={[4, 8, 16, 40].map((n) => ({ value: n, label: `${n} exp` }))} onChange={setExpiries} />
            <Segmented label="Window" value={window} options={[0.02, 0.05, 0.1].map((w) => ({ value: w, label: `±${w * 100}%` }))} onChange={setWindow} />
          </>
        }
      >
        <BarChart bars={totals} markers={markers} height={280} formatX={(x) => fixed(x, 0)} formatY={(y) => money(y)} />
      </Panel>

      <Panel title="By strike and expiry">
        <Heatmap
          label={`${metric === "gex" ? "Gamma" : "Vanna"} exposure by strike and expiry, ${unit}`}
          rows={data.strikes}
          columns={data.expiries.map((e) => ({ id: e.id, label: expiryLabel(e.id) }))}
          values={data.expiries.map((e) => (metric === "gex" ? e.gex : e.vex))}
          highlightRow={nearestStrike}
          format={(v) => money(v)}
          formatRow={(r) => fixed(r, 0)}
        />
        <p className="mt-2 text-[11px] leading-relaxed text-muted">
          Convention: customers are assumed long the open interest, so dealers are long calls and short puts; calls add, puts subtract. That is a
          modelling convention, not knowledge of anyone's positions. GEX is gamma × open interest × multiplier × spot² × 1%; VEX is vanna per
          vol point × open interest × multiplier × spot. Time to expiry is floored at half a day so options minutes from expiry do not dominate.
        </p>
      </Panel>
    </div>
  )
}
