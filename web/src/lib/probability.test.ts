import { describe, expect, it } from "vitest"
import type { OptionQuote } from "../api/types"
import { quote } from "../test/trading-fixtures"
import { probabilityAbove, probabilityOfProfit, singleLeg, smileVol, valueToday, type Distribution } from "./probability"
import { black76, normCdf, payoff, type StrategyLeg } from "./strategy"

const flat = (vol: number, forward = 100, years = 0.25): Distribution => ({ forward, years, vol: () => vol })
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

  it("take the smile's volatility at each breakeven", () => {
    const skew = smileVol([{ strike: 90, iv: 0.3 }, { strike: 100, iv: 0.2 }, { strike: 110, iv: 0.16 }, { strike: 105, iv: null }])
    expect(skew(80)).toBe(0.3)
    expect(skew(95)).toBeCloseTo(0.25, 12)
    expect(skew(120)).toBe(0.16)
    expect(smileVol([], 0.18)(100)).toBe(0.18)
    const put = singleLeg("put", "sell", 95, 1.2)
    expect(probabilityOfProfit(put.value, [put.breakeven], { forward: 100, years: 0.25, vol: skew }))
      .toBeCloseTo(probabilityAbove(93.8, 100, 0.25, skew(93.8))!, 12)
  })

  it("are certain either way without breakevens, and unknown without a volatility", () => {
    expect(probabilityOfProfit(() => 1, [], flat(0.2))).toBe(1)
    expect(probabilityOfProfit(() => -1, [], flat(0.2))).toBe(0)
    const call = singleLeg("call", "buy", 100, 2.5)
    expect(probabilityOfProfit(call.value, [call.breakeven], { forward: 100, years: 0.25, vol: () => null })).toBeNull()
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
