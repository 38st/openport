import { describe, expect, it } from "vitest"
import type { SurfaceExpiry, SviFit } from "../api/types"
import { sampleSvi, smileSeries, sviVol } from "./svi"

const fit: SviFit = { a: .02, b: .1, rho: -.4, m: .02, sigma: .15, rmse_vol_points: .01,
  points: 50, status: "ok", reason: null, fit_ms: 1, butterfly_min_g: .3, butterfly_k: .2, butterfly_ok: true }
const expiry: SurfaceExpiry = { id: "2027-01-15PM", expiry: "2027-01-15", days: 182.5, forward: 5000,
  atm_iv: .2, svi: fit, svi_years: .5, svi_min_k: -.3, svi_max_k: .3,
  points: [-.2, -.05, .2].map((k) => ({ k, strike: 5000 * Math.exp(k), iv: .2, bid_iv: .19, ask_iv: .21 })) }

describe("SVI curves", () => {
  it("evaluates total variance in the correct units and fails closed", () => {
    expect(sviVol(fit, fit.m, .5)).toBeCloseTo(Math.sqrt((fit.a + fit.b * fit.sigma) / .5), 14)
    expect(sviVol(fit, 0, 0)).toBeNull()
    expect(sviVol({ ...fit, a: -1 }, 0, 1)).toBeNull()
    expect(sviVol({ ...fit, sigma: 0 }, 0, 1)).toBeNull()
  })
  it("samples densely and uniformly in k, using the full precision tenor", () => {
    const points = sampleSvi({ ...expiry, days: 180 }, "moneyness")
    expect(points).toHaveLength(241)
    expect(points[0]!.x).toBeCloseTo(-.2)
    expect(points[240]!.x).toBeCloseTo(.2)
    expect(points[120]!.x).toBeCloseTo(0)
    expect(points[120]!.y).toBe(sviVol(fit, 0, .5))
    const strikes = sampleSvi(expiry, "strike")
    expect(strikes[120]!.x).toBeCloseTo(5000)
    expect(strikes[120]!.x - strikes[119]!.x).not.toBeCloseTo(strikes[1]!.x - strikes[0]!.x)
  })
  it("does not extrapolate beyond calibration or invent curves for unavailable fits", () => {
    expect(sampleSvi({ ...expiry, svi: null }, "strike")).toEqual([])
    expect(sampleSvi({ ...expiry, forward: null }, "strike")).toEqual([])
    const limited = sampleSvi({ ...expiry, svi_min_k: -.1, svi_max_k: .1 }, "moneyness")
    expect(limited[0]!.x).toBe(-.1)
    expect(limited[240]!.x).toBe(.1)
  })
  it("draws the fitted curve in the requested window even without displayable market quotes", () => {
    const points = sampleSvi({ ...expiry, points: [] }, "moneyness", { spot: 5000, window: .1 })
    expect(points).toHaveLength(241)
    expect(points[0]!.x).toBeCloseTo(Math.log(.9))
    expect(points[240]!.x).toBeCloseTo(Math.log(1.1))
  })
  it("selects market dots, SVI lines, or both with stable expiry colors", () => {
    const both = smileSeries([expiry], "moneyness", "both")
    expect(both).toHaveLength(2)
    expect(both[0]!.dots).toBe(true)
    expect(both[0]!.band).toBe(true)
    expect(both[1]!.dots).toBeUndefined()
    expect(both[1]!.points.length).toBe(241)
    expect(both[0]!.color).toBe(both[1]!.color)
    expect(smileSeries([expiry], "strike", "market").map((s) => s.id)).toEqual([`${expiry.id}-market`])
    expect(smileSeries([expiry], "strike", "svi").map((s) => s.id)).toEqual([`${expiry.id}-svi`])
  })
})
