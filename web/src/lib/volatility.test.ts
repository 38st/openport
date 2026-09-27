import { describe, expect, it } from "vitest"
import type { Volatility, VolCone } from "../api/types"
import { coneSeries, metricMark, volatilityTerm, volPoints } from "./volatility"

describe("volatility metric presentation", () => {
  it("preserves vol-point units and distinguishes missing from zero", () => {
    expect(volPoints(20)).toBe("20.00")
    expect(volPoints(0)).toBe("0.00")
    for (const value of [null, undefined, NaN, Infinity]) expect(volPoints(value)).toBe("—")
  })
  it("marks truncation independently of proxies and realized fallbacks", () => {
    expect(metricMark({ truncated: true, proxy: true })).toBe(" † ≈")
    expect(metricMark({ fallback: true })).toBe(" ≈")
    expect(metricMark()).toBe("")
  })
  it("orders the term structure on square-root calendar time without rescaling IV", () => {
    const data = { mfiv: { expiries: [
      { minutes: 30 * 1440, vol: 20, truncated: true },
      { minutes: 9 * 1440, vol: null, truncated: false },
      { minutes: -1440, vol: 30, truncated: false },
    ] }, atm: { expiries: [{ minutes: 30 * 1440, vol: 19 }] } } as Volatility
    const series = volatilityTerm(data)
    expect(series[0]!.points).toEqual([{ x: 3, y: null }, { x: Math.sqrt(30), y: 20 }])
    expect(series[1]!.points[0]!.y).toBe(19)
    expect(series[2]!.points).toEqual([{ x: Math.sqrt(30), y: 20 }])
  })
  it("uses supplied cone percentiles and exact session-horizon implied markers", () => {
    const cones = [{ sessions: 21, min: 10, p10: 12, p25: 14, p50: 16, p75: 18, p90: 20, max: 24, current: 17, implied_vol: null }] as VolCone[]
    const series = coneSeries(cones)
    expect(series).toHaveLength(4)
    expect(series[0]!.points[0]).toEqual({ x: 21, y: 16, lo: 12, hi: 20 })
    expect(series[1]!.points[0]).toEqual({ x: 21, y: null, lo: 14, hi: 18 })
    expect(series[2]!.points[0]).toEqual({ x: 21, y: 17 })
    expect(series[3]!.points[0]).toEqual({ x: 21, y: null })
    // The extremes stay out of the chart so they cannot set its scale.
    expect(series.flatMap(s => s.points).some(p => "hi" in p && p.hi === 24)).toBe(false)
  })
})
