import type { SurfaceExpiry, SviFit } from "../api/types"
import type { Series } from "../charts/LineChart"
import { days, expiryLabel, isNum } from "./format"

interface DisplayRange { spot: number | null; window: number }

export type SmileMode = "market" | "svi" | "both"
export const smileColor = (i: number) => `var(--chart-${i % 8 + 1})`

export function sviVol(p: SviFit, k: number, years: number): number | null {
  if (!(years > 0) || !Number.isFinite(years) || !(p.sigma > 0) || p.b < 0 || Math.abs(p.rho) >= 1) return null
  const x = k - p.m
  const w = p.a + p.b * (p.rho * x + Math.hypot(x, p.sigma))
  return w >= 0 && Number.isFinite(w) ? Math.sqrt(w / years) : null
}

/** Uniform in k, clipped to the selected window and calibration range.
 * Sampling is independent of quoted strike spacing, including on a strike axis. */
export function sampleSvi(e: SurfaceExpiry, axis: "strike" | "moneyness", display?: DisplayRange): Series["points"] {
  if (!e.svi || e.svi.status !== "ok" || !isNum(e.forward) || e.forward <= 0) return []
  const ks = e.points.map((p) => p.k).filter(isNum)
  const hasWindow = isNum(display?.spot) && display.spot > 0 && display.window > 0 && display.window < 1
  if (!hasWindow && ks.length < 2) return []
  const windowLo = hasWindow ? Math.log(display.spot! * (1 - display.window) / e.forward) : Math.min(...ks)
  const windowHi = hasWindow ? Math.log(display.spot! * (1 + display.window) / e.forward) : Math.max(...ks)
  const lo = Math.max(windowLo, e.svi_min_k ?? -Infinity)
  const hi = Math.min(windowHi, e.svi_max_k ?? Infinity)
  if (!(hi > lo)) return []
  const years = e.svi_years ?? (e.days ?? 0) / 365
  if (!(years > 0) || !Number.isFinite(years)) return []
  return Array.from({ length: 241 }, (_, i) => {
    const k = lo + (hi - lo) * i / 240
    return { x: axis === "strike" ? e.forward! * Math.exp(k) : k, y: sviVol(e.svi!, k, years) }
  })
}

export function smileSeries(expiries: SurfaceExpiry[], axis: "strike" | "moneyness", mode: SmileMode, display?: DisplayRange): Series[] {
  return expiries.flatMap((e, i) => {
    const base = { label: `${expiryLabel(e.id)} · ${days(e.days)}`, color: smileColor(i) }
    const series: Series[] = []
    if (mode !== "svi") series.push({ ...base, id: `${e.id}-market`, label: `${base.label} market`, dots: true, band: i === 0,
      points: e.points.filter((p) => axis === "strike" || isNum(p.k)).map((p) => ({
        x: axis === "strike" ? p.strike : p.k!, y: p.iv, lo: p.bid_iv, hi: p.ask_iv,
      })) })
    if (mode !== "market") {
      const points = sampleSvi(e, axis, display)
      if (points.length) series.push({ ...base, id: `${e.id}-svi`, label: `${base.label} SVI`, points })
    }
    return series
  })
}
