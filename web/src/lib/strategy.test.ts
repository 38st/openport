import { describe, expect, it } from "vitest"
import { quote } from "../test/trading-fixtures"
import { legsLabel, orderLabel } from "./journal"
import { netQuote, riskProfile, roundNet, strategyBuyingPowerEffect, strategyLabel, strategyRequirement, toggleLeg, type StrategyLeg } from "./strategy"
import { comboTickCents } from "./trading"

const osi = (type: "C" | "P", strike: number, date = "261022") => `SPXW  ${date}${type}${String(strike * 1000).padStart(8, "0")}`
function leg(type: "call" | "put", strike: number, side: "buy" | "sell", bid: number, ask: number, ratio = 1, expiry = "2026-10-22PM"): StrategyLeg {
  return { symbol: osi(type === "call" ? "C" : "P", strike, expiry.startsWith("2026-10-22") ? "261022" : "261023"), side, ratio, type, strike, expiry,
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
  it("mirrors the server's buying power for a strategy by itself", () => {
    // The credit spread holds its width; the reservation takes the credit off and adds fees.
    expect(strategyRequirement(putSpread, 3, 5000)).toBe(3000)
    expect(strategyBuyingPowerEffect(putSpread, 3, -1.2, 0.65, 5000)).toBeCloseTo(-2643.9, 6)
    // A debit spread reserves its debit.
    const debit = [leg("put", 4900, "buy", 5, 5.2), leg("put", 4890, "sell", 4, 4.2)]
    expect(strategyRequirement(debit, 1, 5000)).toBe(0)
    expect(strategyBuyingPowerEffect(debit, 1, 1.2, 0.65, 5000)).toBeCloseTo(-121.3, 6)
    // A short strangle is naked on both sides: mid buy-back plus 20% of spot less the OTM amount.
    const strangle = [leg("call", 5100, "sell", 3, 3.2), leg("put", 4900, "sell", 5, 5.2)]
    expect(strategyRequirement(strangle, 1, 5000)).toBeCloseTo(310 + 90000 + 510 + 90000, 6)
  })
  it("labels multi-leg orders compactly", () => {
    const legs = [{ symbol: osi("P", 4900), side: "sell" as const, ratio: 1 }, { symbol: osi("P", 4890), side: "buy" as const, ratio: 2 }]
    expect(legsLabel(legs, "SPX")).toBe("SPX Oct 22 −4900P +2×4890P")
    expect(legsLabel([legs[0]!, { symbol: osi("P", 4900, "261023"), side: "buy", ratio: 1 }], "SPX")).toBe("SPX −Oct 22 4900P +Oct 23 4900P")
    expect(orderLabel({ symbol: null, underlying: "SPX", legs })).toBe("SPX Oct 22 −4900P +2×4890P")
    expect(orderLabel({ symbol: osi("C", 5000), underlying: "SPX", legs: null })).toBe("SPX Oct 22 5000C")
  })
})
