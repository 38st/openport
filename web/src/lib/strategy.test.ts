import { describe, expect, it } from "vitest"
import { quote } from "../test/trading-fixtures"
import { legsLabel, orderLabel } from "./journal"
import { black76, closingLegs, estimatedProfile, netQuote, normCdf, riskProfile, roundNet, strategyLabel, strategyPayoff, strategyPowerUse, toggleLeg, type ExpiryTerms, type StrategyLeg } from "./strategy"
import { comboTickCents } from "./trading"

const osi = (type: "C" | "P", strike: number, date = "261022") => `SPXW  ${date}${type}${String(strike * 1000).padStart(8, "0")}`
function leg(type: "call" | "put", strike: number, side: "buy" | "sell", bid: number, ask: number, ratio = 1, expiry = "2026-10-22PM"): StrategyLeg {
  return { symbol: osi(type === "call" ? "C" : "P", strike, expiry.startsWith("2026-10-22") ? "261022" : "261023"), underlying: "SPX", side, ratio, type, strike, expiry,
    quote: { ...quote, bid, ask, mid: (bid + ask) / 2, tradable: true } }
}
const putSpread = [leg("put", 4900, "sell", 5, 5.2), leg("put", 4890, "buy", 4, 4.2)]

describe("strategies", () => {
  it("adds, flips and removes legs from chain clicks, up to four", () => {
    const [a, b] = putSpread as [StrategyLeg, StrategyLeg]
    let legs = toggleLeg([], a)
    legs = toggleLeg(legs, b)
    expect(legs.map((l) => l.side)).toEqual(["sell", "buy"])
    legs = toggleLeg(legs, { ...b, side: "sell" })
    expect(legs[1]!.side).toBe("sell")
    expect(toggleLeg(legs, { ...b, side: "sell" })).toHaveLength(1)
    const full = [leg("put", 4800, "buy", 1, 1.1), leg("put", 4810, "buy", 1, 1.1), leg("put", 4820, "buy", 1, 1.1), leg("put", 4830, "buy", 1, 1.1)]
    expect(toggleLeg(full, a)).toBe(full)
  })
  it("prices the net per unit from every leg's far and near sides", () => {
    expect(netQuote(putSpread)).toEqual({ bid: -1.2, mid: -1, ask: -0.8 })
    expect(netQuote([leg("put", 4900, "sell", 5, 5.2), leg("put", 4890, "buy", 4, 4.2, 2)])).toEqual({ bid: 2.8, mid: 3.1, ask: 3.4 })
    expect(netQuote([{ ...putSpread[0]!, quote: null }, putSpread[1]!])).toEqual({ bid: null, mid: null, ask: null })
    expect(roundNet(-0.83, 5)).toBe(-0.85)
    expect(roundNet(1.22, 5)).toBe(1.2)
    expect(comboTickCents(["SPXW", "SPXW"])).toBe(5)
    expect(comboTickCents(["SPY", "SPY"])).toBe(1)
    expect(comboTickCents(["SPXW", "XSP"])).toBe(1)
  })
  it("names common strategies", () => {
    expect(strategyLabel(putSpread)).toBe("Bull put spread")
    expect(strategyLabel([leg("put", 4900, "buy", 5, 5.2), leg("put", 4890, "sell", 4, 4.2)])).toBe("Bear put spread")
    expect(strategyLabel([leg("call", 5100, "buy", 3, 3.2), leg("call", 5110, "sell", 2, 2.2)])).toBe("Bull call spread")
    expect(strategyLabel([leg("call", 5100, "sell", 3, 3.2), leg("call", 5110, "buy", 2, 2.2)])).toBe("Bear call spread")
    expect(strategyLabel([leg("call", 5000, "buy", 3, 3.2), leg("put", 5000, "buy", 3, 3.2)])).toBe("Long straddle")
    expect(strategyLabel([leg("call", 5100, "sell", 3, 3.2), leg("put", 4900, "sell", 3, 3.2)])).toBe("Short strangle")
    expect(strategyLabel([leg("call", 5100, "buy", 3, 3.2), leg("put", 4900, "sell", 3, 3.2)])).toBe("Risk reversal")
    expect(strategyLabel([...putSpread, leg("call", 5100, "sell", 3, 3.2), leg("call", 5110, "buy", 2, 2.2)])).toBe("Iron condor")
    expect(strategyLabel([leg("put", 4990, "buy", 1, 1.2), leg("put", 5000, "sell", 5, 5.2), leg("call", 5000, "sell", 5, 5.2), leg("call", 5010, "buy", 1, 1.2)])).toBe("Iron butterfly")
    expect(strategyLabel([leg("call", 5100, "buy", 3, 3.2), leg("call", 5110, "sell", 2, 2.2, 2), leg("call", 5120, "buy", 1, 1.2)])).toBe("Long call butterfly")
    expect(strategyLabel([leg("put", 4900, "sell", 5, 5.2), leg("put", 4900, "buy", 6, 6.2, 1, "2026-10-23PM")])).toBe("Calendar spread")
    expect(strategyLabel([leg("put", 4900, "sell", 5, 5.2), leg("put", 4890, "sell", 4, 4.2)])).toBe("Custom · 2 legs")
  })
  it("reports the expiry risk: max profit, max loss and breakevens", () => {
    expect(riskProfile(putSpread, 2, -0.8)).toEqual({ maxProfit: 160, maxLoss: 1840, breakevens: [4899.2] })
    const straddle = [leg("call", 5000, "buy", 30, 30.2), leg("put", 5000, "buy", 28, 28.2)]
    expect(riskProfile(straddle, 1, 58.4)).toEqual({ maxProfit: null, maxLoss: 5840, breakevens: [4941.6, 5058.4] })
    const condor = [...putSpread, leg("call", 5100, "sell", 3, 3.2), leg("call", 5110, "buy", 2, 2.2)]
    expect(riskProfile(condor, 1, -1.8)).toEqual({ maxProfit: 180, maxLoss: 820, breakevens: [4898.2, 5101.8] })
    expect(riskProfile([leg("call", 5100, "sell", 3, 3.2), leg("call", 5110, "buy", 2, 2.2, 1, "2026-10-23PM")], 1, -1)).toBeNull()
    expect(riskProfile([leg("call", 5100, "sell", 3, 3.2), leg("put", 4900, "sell", 3, 3.2)], 1, -6)!.maxLoss).toBeNull()
  })
  it("mirrors the server's buying power for a strategy on the held positions", () => {
    // The credit spread holds its width; the reservation takes the credit off and adds fees.
    expect(strategyPowerUse(putSpread, 3, -1.2, 0.65, 5000)).toEqual({ effect: expect.closeTo(-2643.9, 6), uses: true })
    // A debit spread reserves its debit.
    const debit = [leg("put", 4900, "buy", 5, 5.2), leg("put", 4890, "sell", 4, 4.2)]
    expect(strategyPowerUse(debit, 1, 1.2, 0.65, 5000)!.effect).toBeCloseTo(-121.3, 6)
    // A short strangle is naked on both sides: mid buy-back plus 20% of spot less the OTM amount.
    const strangle = [leg("call", 5100, "sell", 3, 3.2), leg("put", 4900, "sell", 5, 5.2)]
    expect(strategyPowerUse(strangle, 1, 0, 0, 5000)!.effect).toBeCloseTo(-(310 + 90000 + 510 + 90000), 6)
    // Closing a held spread together frees its width: only fees.
    const held = [{ symbol: putSpread[0]!.symbol, underlying: "SPX", expiry: "2026-10-22PM", type: "put" as const, strike: 4900, quantity: -1, value: 510 },
      { symbol: putSpread[1]!.symbol, underlying: "SPX", expiry: "2026-10-22PM", type: "put" as const, strike: 4890, quantity: 1, value: 0 }]
    const close = [{ ...putSpread[0]!, side: "buy" as const }, { ...putSpread[1]!, side: "sell" as const }]
    expect(strategyPowerUse(close, 1, 1.2, 0.65, 5000, held)).toEqual({ effect: expect.closeTo(-1.3, 6), uses: false })
  })
  it("labels multi-leg orders compactly", () => {
    const legs = [{ symbol: osi("P", 4900), side: "sell" as const, ratio: 1 }, { symbol: osi("P", 4890), side: "buy" as const, ratio: 2 }]
    expect(legsLabel(legs, "SPX")).toBe("SPX Oct 22 −4900P +2×4890P")
    expect(legsLabel([legs[0]!, { symbol: osi("P", 4900, "261023"), side: "buy", ratio: 1 }], "SPX")).toBe("SPX −Oct 22 4900P +Oct 23 4900P")
    expect(orderLabel({ symbol: null, underlying: "SPX", legs })).toBe("SPX Oct 22 −4900P +2×4890P")
    expect(orderLabel({ symbol: osi("C", 5000), underlying: "SPX", legs: null })).toBe("SPX Oct 22 5000C")
  })
  it("prices later legs with Black-76 to estimate a calendar at the first expiry", () => {
    expect(normCdf(0)).toBeCloseTo(0.5, 7)
    expect(normCdf(1.96)).toBeCloseTo(0.975, 3)
    expect(black76("call", 100, 100, 0.2, 1, 1)).toBeCloseTo(7.9656, 3)
    // Put-call parity: C - P = D (F - K).
    expect(black76("call", 105, 100, 0.3, 0.5, 0.98) - black76("put", 105, 100, 0.3, 0.5, 0.98)).toBeCloseTo(0.98 * 5, 9)
    expect(black76("put", 90, 100, 0, 1, 1)).toBe(10)
    const near = Date.parse("2026-10-22T20:00:00Z"), day = 86_400_000
    const terms = new Map<string, ExpiryTerms>([["2026-10-22PM", { id: "2026-10-22PM", time: near, forward: 5000, discount: 0.99 }],
      ["2026-10-29PM", { id: "2026-10-29PM", time: near + 7 * day, forward: 5000, discount: 0.99 }]])
    const later = { ...leg("put", 4900, "buy", 8, 8.4, 1, "2026-10-29PM"), expiry: "2026-10-29PM", quote: { ...quote, bid: 8, ask: 8.4, mid: 8.2, iv: 0.2, tradable: true } }
    const calendar = [leg("put", 4900, "sell", 5, 5.2), later]
    const value = strategyPayoff(calendar, 1, 3, terms)!
    // At the short strike the near put expires worthless while the later put keeps a week of time value.
    expect(value(4900)).toBeCloseTo(100 * black76("put", 4900, 4900, 0.2, 7 / 365.25, 1) - 300, 6)
    // Far from the strike both puts are worth their intrinsic value, so the loss is the debit.
    expect(value(3500)).toBeCloseTo(-300, 0)
    const profile = estimatedProfile(value, calendar, 1, 3430, 6500)
    expect(profile.estimated).toBe(true)
    expect(profile.maxLoss).toBeCloseTo(300, 0)
    expect(profile.maxProfit).toBeGreaterThan(value(4900) - 50)
    expect(profile.maxProfit).toBeLessThanOrEqual(value(4900) + 1e-9)
    expect(profile.breakevens).toHaveLength(2)
    expect(profile.breakevens[0]).toBeLessThan(4900)
    expect(profile.breakevens[1]).toBeGreaterThan(4900)
    expect(strategyPayoff([calendar[0]!, { ...later, quote: { ...later.quote, iv: null } }], 1, 3, terms)).toBeNull()
    expect(strategyPayoff(calendar, 1, 3, new Map())).toBeNull()
  })
  it("reverses held positions into one closing order with ratios per unit", () => {
    const position = (symbol: string, type: "call" | "put", strike: number, quantity: number, underlying = "SPX") =>
      ({ symbol, underlying, expiry: "2026-10-22", settlement: "PM" as const, type, strike, quantity })
    const spread = closingLegs([position(osi("P", 4900), "put", 4900, -2), position(osi("P", 4890), "put", 4890, 4)])
    expect(spread).toEqual({ units: 2, legs: [
      expect.objectContaining({ symbol: osi("P", 4900), side: "buy", ratio: 1, expiry: "2026-10-22PM" }),
      expect.objectContaining({ symbol: osi("P", 4890), side: "sell", ratio: 2 })] })
    expect(closingLegs([position(osi("P", 4900), "put", 4900, -1)])).toEqual({ reason: "Choose two to 4 positions." })
    expect(closingLegs([position(osi("P", 4900), "put", 4900, -1), position("SPY   261022P00490000", "put", 490, 1, "SPY")]))
      .toEqual({ reason: "Positions closed together must share one underlying." })
    expect("reason" in closingLegs([position(osi("P", 4900), "put", 4900, -1), position(osi("P", 4890), "put", 4890, 11)])).toBe(true)
  })
})
