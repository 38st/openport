import { describe, expect, it } from "vitest"
import type { OptionQuote, Surface, SviFit } from "../api/types"
import { expiry, quote } from "../test/trading-fixtures"
import { probabilityAbove, probabilityOfProfit, probabilitySource, smileDistribution, singleLeg, smileVol, valueToday, type Distribution } from "./probability"
import { sviVol, ssviVol } from "./svi"
import { black76, normCdf, payoff, riskProfile, type StrategyLeg } from "./strategy"

const flat = (vol: number, forward = 100, years = 0.25): Distribution => ({ forward, years, vol: () => vol, above: (k) => probabilityAbove(k, forward, years, vol) })
const leg = (strike: number, type: "call" | "put", side: "buy" | "sell", iv = 0.2): StrategyLeg =>
  ({ symbol: `X${strike}${type}`, underlying: "SPX", side, ratio: 1, type, strike, expiry: "2026-10-16PM", quote: { ...quote, iv } as OptionQuote })

describe("probabilities at expiry", () => {
  it("are lognormal around the forward", () => {
    expect(probabilityAbove(100, 100, 1, 0.2)).toBeCloseTo(normCdf(-0.1), 12)  // a touch under a half: the median sits below the forward
    expect(probabilityAbove(0, 100, 1, 0.2)).toBe(1)
    expect(probabilityAbove(120, 100, 1, null)).toBeNull()
    expect(probabilityAbove(120, 100, 0, 0.2)).toBeNull()
  })

  it("of profit follow the breakevens: above for a long call, between them for a condor", () => {
    const call = singleLeg("call", "buy", 100, 2.5)
    expect(call.breakeven).toBe(102.5)
    expect(probabilityOfProfit(call.value, [call.breakeven], flat(0.2))).toBeCloseTo(probabilityAbove(102.5, 100, 0.25, 0.2)!, 12)
    const put = singleLeg("put", "sell", 95, 1.2)
    expect(put.breakeven).toBe(93.8)
    expect(probabilityOfProfit(put.value, [put.breakeven], flat(0.2))).toBeCloseTo(probabilityAbove(93.8, 100, 0.25, 0.2)!, 12)
    const condor = [leg(90, "put", "buy"), leg(95, "put", "sell"), leg(105, "call", "sell"), leg(110, "call", "buy")]
    const value = (s: number) => payoff(condor, 1, -2, s)  // a $2 credit
    const inside = probabilityOfProfit(value, [93, 107], flat(0.2))!
    expect(inside).toBeCloseTo(probabilityAbove(93, 100, 0.25, 0.2)! - probabilityAbove(107, 100, 0.25, 0.2)!, 12)
    expect(inside).toBeGreaterThan(0.5)
  })

  it("use the chain smile's local skew at each breakeven", () => {
    const rows = [{ strike: 90, iv: 0.3 }, { strike: 100, iv: 0.2 }, { strike: 110, iv: 0.16 }, { strike: 105, iv: null }]
    const skew = smileVol(rows)
    expect(skew(80)).toBe(0.3)
    expect(skew(95)).toBeCloseTo(0.3 - 0.1 * Math.log(95 / 90) / Math.log(100 / 90), 12)
    expect(skew(120)).toBe(0.16)
    expect(smileVol([], 0.18)(100)).toBe(0.18)
    const put = singleLeg("put", "sell", 95, 1.2)
    const distribution = smileDistribution({ ...expiry, forward: 100, days: 365 / 4 }, rows)!
    expect(probabilityOfProfit(put.value, [put.breakeven], distribution)).toBe(distribution.above(93.8))
    expect(distribution.above(93.8)!).toBeGreaterThan(probabilityAbove(93.8, 100, .25, skew(93.8))!)
  })

  it("are certain either way without breakevens, and unknown without a volatility", () => {
    expect(probabilityOfProfit(() => 1, [], flat(0.2))).toBe(1)
    expect(probabilityOfProfit(() => -1, [], flat(0.2))).toBe(0)
    const call = singleLeg("call", "buy", 100, 2.5)
    expect(probabilityOfProfit(call.value, [call.breakeven], { forward: 100, years: 0.25, vol: () => null, above: () => null })).toBeNull()
  })
})

describe("the value of a strategy today", () => {
  it("prices every leg at its own volatility and time left, carried from spot", () => {
    const legs = [leg(100, "call", "buy", 0.2), leg(105, "call", "sell", 0.18)]
    const terms = new Map([["2026-10-16PM", { forward: 101, discount: 0.99, years: 0.1 }]])
    const today = valueToday(legs, 2, 1.5, 100, terms)!
    const expected = 2 * 100 * (black76("call", 101, 100, 0.2, 0.1, 0.99) - black76("call", 101, 105, 0.18, 0.1, 0.99)) - 1.5 * 100 * 2
    expect(today(100)).toBeCloseTo(expected, 9)
    // Spot 2% higher carries the forward with it.
    expect(today(102)).toBeCloseTo(2 * 100 * (black76("call", 103.02, 100, 0.2, 0.1, 0.99) - black76("call", 103.02, 105, 0.18, 0.1, 0.99)) - 300, 9)
    expect(valueToday([leg(100, "call", "buy", Number.NaN)], 1, 1, 100, terms)).toBeNull()
    expect(valueToday(legs, 1, 1, 100, new Map())).toBeNull()
  })
})


const skewFit: SviFit = { a: .004, b: .025, rho: -.7, m: 0, sigma: .1, status: "ok", reason: null,
  points: 50, fit_ms: 1, rmse_vol_points: .1, butterfly_ok: true, butterfly_min_g: .2, butterfly_k: 0 }
const terms = { ...expiry, forward: 100, days: 73, atm_iv: .2 }
const surface: Surface = { symbol: "SPX", spot: 100, as_of: null, version: 1, expiries: [{
  id: terms.id, expiry: terms.expiry, forward: 100, days: 73, atm_iv: .2, points: [], svi: skewFit,
  svi_years: .2, svi_min_k: -1, svi_max_k: 1, ssvi_theta: .008, ssvi_min_k: -1, ssvi_max_k: 1,
}], ssvi: { status: "ok", rho: -.6, eta: .7, gamma: .3, reason: null, fit_ms: 1, monotone_adjusted: false, rmse_vol_points: .1 } }
const withoutSvi = { ...surface, expiries: surface.expiries.map((e) => ({ ...e, svi: null })) }
const callAt = (k: number) => black76("call", 100, k, sviVol(skewFit, Math.log(k / 100), .2)!, .2, .99)

describe("smile risk-neutral distributions", () => {
  it("gives N(d2) exactly for a flat chain smile and a flat SVI fit", () => {
    const rows = [80, 90, 100, 110, 120].map((strike) => ({ strike, iv: .2 }))
    const fitted = { ...surface, expiries: surface.expiries.map((e) => ({ ...e, svi: { ...skewFit, a: .008, b: 0 } })) }
    for (const d of [smileDistribution(terms, rows)!, smileDistribution(terms, [], fitted)!]) {
      for (const k of [20, 85.321, 95, 100, 105.567, 140]) expect(d.above(k)).toBe(probabilityAbove(k, 100, .2, d.vol(k)))
      expect(d.repaired).toBe(false)
    }
  })
  it("includes the negative-skew term and matches finite-difference Black-76 digitals", () => {
    const d = smileDistribution(terms, [], surface)!
    expect(d.source).toBe("svi")
    for (const k of [95, 100, 105]) {
      expect(d.above(k)!).toBeGreaterThan(probabilityAbove(k, 100, .2, d.vol(k))!)
      const h = .001
      const digital = -(callAt(k + h) - callAt(k - h)) / (2 * h * .99)
      expect(d.above(k)).toBeCloseTo(digital, 5)
    }
    expect(d.above(100)! - probabilityAbove(100, 100, .2, d.vol(100))!).toBeGreaterThan(.03)
  })
  it("chooses checked SVI, then SSVI, then chain data, then a labelled lognormal", () => {
    const rows = [{ strike: 80, iv: .25 }, { strike: 100, iv: .2 }, { strike: 120, iv: .18 }]
    expect(smileDistribution(terms, rows, surface)!.source).toBe("svi")
    const ssvi = smileDistribution(terms, rows, withoutSvi)!
    expect(ssvi.source).toBe("ssvi")
    const k = 99, h = .001
    const price = (strike: number) => black76("call", 100, strike, ssviVol(surface.ssvi!, Math.log(strike / 100), .008, .2)!, .2, .99)
    expect(ssvi.above(k)).toBeCloseTo(probabilityAbove(k, 100, .2, ssvi.vol(k), ssvi.slope(k)!)!, 5)
    expect(ssvi.above(k)).toBeCloseTo(-(price(k + h) - price(k - h)) / (2 * h * .99), 4)
    expect(smileDistribution(terms, rows, { ...surface, expiries: surface.expiries.map((e) => ({ ...e, svi: { ...skewFit, butterfly_ok: false } })) })!.source).toBe("ssvi")
    expect(smileDistribution(terms, rows, { ...surface, calendar_violations: [{ earlier: terms.id, later: "later", k: 0 }] })!.source).toBe("ssvi")
    expect(smileDistribution(terms, rows, { ...withoutSvi, ssvi: undefined })!.source).toBe("chain")
    expect(smileDistribution(terms, rows)!.source).toBe("chain")
    expect(probabilitySource(smileDistribution(terms)!, [100])).toBe("lognormal, no smile")
    expect(smileDistribution({ ...terms, days: 0 }, rows)).toBeNull()
    expect(smileDistribution({ ...terms, forward: NaN }, rows)).toBeNull()
    expect(smileDistribution({ ...terms, atm_iv: null })!.above(100)).toBeNull()
  })
  it("uses precise fitted terms when the chain's rounded tenor is zero", () => {
    const precise = { ...surface, expiries: surface.expiries.map((e) => ({ ...e, days: 0, svi_years: 1e-6,
      svi: { ...skewFit, a: 2e-8, b: 1e-8 } })) }
    const d = smileDistribution({ ...terms, days: 0, forward: null }, [], precise)!
    expect(d.source).toBe("svi")
    expect(d.forward).toBe(100)
    expect(d.years).toBe(1e-6)
    expect(d.above(100)).toBe(smileDistribution({ ...terms, days: 365e-6 }, [], precise)!.above(100))
  })
  it("ignores other expiries and invalid fits or ranges", () => {
    const rows = [{ strike: 80, iv: .25 }, { strike: 100, iv: .2 }, { strike: 120, iv: .18 }]
    const invalid = { ...surface, ssvi: undefined }
    for (const e of [
      { ...surface.expiries[0]!, id: "another-expiry" },
      { ...surface.expiries[0]!, svi_min_k: 1, svi_max_k: -1 },
      { ...surface.expiries[0]!, svi: { ...skewFit, a: NaN } },
      { ...surface.expiries[0]!, svi_years: 0 },
    ]) expect(smileDistribution(terms, rows, { ...invalid, expiries: [e] })!.source).toBe("chain")
    expect(smileDistribution(terms, rows, { ...withoutSvi, ssvi: { ...surface.ssvi!, status: "failed" } })!.source).toBe("chain")
  })
  it("fits local chain skew in log moneyness and averages duplicate strikes", () => {
    const rows = [80, 90, 100, 110, 120].map((strike) => ({ strike, iv: .2 - .1 * Math.log(strike / 100) }))
    const d = smileDistribution(terms, [...rows, rows[2]!, { strike: -1, iv: .4 }, { strike: 100, iv: NaN }])!
    expect(d.vol(97)).toBeCloseTo(.2 - .1 * Math.log(.97), 12)
    expect(d.slope(97)).toBeCloseTo(-.1, 12)
    expect(d.above(100)!).toBeGreaterThan(probabilityAbove(100, 100, .2, .2)!)
    const bump = smileDistribution(terms, rows.map((r) => ({ ...r, iv: r.iv + (r.strike === 100 ? .002 : 0) })))!
    expect(Math.abs(bump.slope(100)! + .1)).toBeLessThan(.001)
  })
  it("holds volatility flat outside the fit and discloses extrapolation", () => {
    const d = smileDistribution(terms, [], surface)!
    expect(d.vol(1)).toBe(d.vol(d.range![0]))
    expect(d.vol(1000)).toBe(d.vol(d.range![1]))
    expect(d.slope(1)).toBe(0)
    expect(d.slope(1000)).toBe(0)
    expect(probabilitySource(d, [1])).toContain("volatility held flat beyond the smile")
    expect(probabilitySource(d, [100])).not.toContain("held flat")
  })
  it("clamps and repairs an arbitrage smile to a monotone CDF independent of query order", () => {
    const rows = [70, 80, 90, 100, 110, 120, 130].map((strike, i) => ({ strike, iv: [.1, .1, .8, .01, .8, .1, .1][i]! }))
    const d = smileDistribution(terms, rows)!
    expect(d.repaired).toBe(true)
    expect(probabilitySource(d, [100])).toContain("approximate: monotonicity repaired")
    const strikes = Array.from({ length: 10001 }, (_, i) => 1 + i * .04)
    const reversed = [...strikes].reverse().map(d.above).reverse()
    let previous = 1
    for (const [i, strike] of strikes.entries()) {
      const p = d.above(strike)!
      expect(p).toBeGreaterThanOrEqual(0)
      expect(p).toBeLessThanOrEqual(previous)
      expect(p).toBe(reversed[i])
      previous = p
    }
    expect(d.above(0)).toBe(1)
    expect(d.above(Infinity)).toBe(0)
  })
  it.each([
    { name: "short put spread", legs: [leg(90, "put", "buy"), leg(95, "put", "sell")], net: -1.2 },
    { name: "long call", legs: [leg(100, "call", "buy")], net: 2.5 },
    { name: "iron condor", legs: [leg(90, "put", "buy"), leg(95, "put", "sell"), leg(105, "call", "sell"), leg(110, "call", "buy")], net: -2 },
  ])("integrates profitable regions for a $name against the call-price density", ({ legs, net }) => {
    const d = smileDistribution(terms, [], surface)!
    const value = (s: number) => payoff(legs, 1, net, s)
    const profile = riskProfile(legs, 1, net)!
    // Independent midpoint quadrature of C'' / DF, from the fitted call prices.
    const step = .002, h = .01
    let integrated = 0
    for (let s = 40 + step / 2; s < 220; s += step) {
      if (value(s) <= 0) continue
      integrated += (callAt(s + h) - 2 * callAt(s) + callAt(s - h)) / (h * h * .99) * step
    }
    expect(probabilityOfProfit(value, profile.breakevens, d)).toBeCloseTo(integrated, 4)
  })
})
