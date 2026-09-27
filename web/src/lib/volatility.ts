import type { Volatility, VolCone } from "../api/types"
import type { Series } from "../charts/LineChart"

export function volPoints(value: number | null | undefined, digits = 2): string {
  return value == null || !Number.isFinite(value) ? "—" : value.toFixed(digits)
}
export function metricMark(value?: { truncated?: boolean; proxy?: boolean; fallback?: boolean }): string {
  return `${value?.truncated ? " †" : ""}${value?.proxy || value?.fallback ? " ≈" : ""}`
}
export function volatilityTerm(data: Volatility): Series[] {
  return [
    { id: "mfiv", label: "Model-free IV", color: "var(--chart-1)", points: data.mfiv.expiries.map(e => ({ x: Math.sqrt(e.minutes / 1440), y: e.vol })) },
    { id: "atm", label: "ATM IV", color: "var(--chart-2)", points: data.atm.expiries.map(e => ({ x: Math.sqrt(e.minutes / 1440), y: e.vol })) },
    { id: "truncated", label: "Truncated model-free IV †", color: "var(--warn)", dots: true,
      points: data.mfiv.expiries.filter(e => e.truncated).map(e => ({ x: Math.sqrt(e.minutes / 1440), y: e.vol })) },
  ].map(s => ({ ...s, points: s.points.filter(p => Number.isFinite(p.x) && p.x > 0).sort((a, b) => a.x - b.x) }))
}
/**
 * Bands from the 10th to 90th and 25th to 75th percentiles around the median. The
 * extremes stay in the table: a crash's 150-vol week would flatten every band.
 */
export function coneSeries(cones: VolCone[]): Series[] {
  return [
    { id: "p10-90", label: "Median / 10th–90th", color: "var(--chart-1)", band: true,
      points: cones.map(c => ({ x: c.sessions, y: c.p50, lo: c.p10, hi: c.p90 })) },
    { id: "p25-75", label: "25th–75th", color: "var(--chart-1)", band: true,
      points: cones.map(c => ({ x: c.sessions, y: null, lo: c.p25, hi: c.p75 })) },
    { id: "current", label: "Current RV", color: "var(--chart-2)", dots: true,
      points: cones.map(c => ({ x: c.sessions, y: c.current })) },
    { id: "implied", label: "ATM IV at session horizon", color: "var(--chart-3)", dots: true,
      points: cones.map(c => ({ x: c.sessions, y: c.implied_vol })) },
  ]
}
