import { describe, expect, it } from "vitest"
import { portfolio, quote, trades } from "../test/trading-fixtures"
import { signedPercent } from "./format"
import { contractLabel, dailyResults, formatDuration, journalStats, monthWeeks, newYorkDate, osiLabel, parseOsi, tradeBuckets } from "./journal"
import { heldPositions, marginRequirement, orderPowerUse, type MarginPosition } from "./margin"
import { crossDirection, describeTrigger, marketability, nakedRequirement, opposite, split, stopDirection, strategyName } from "./ticket"
import { ratio, roundToTick, signedMoney, stepLimitPrice, subtractMoney } from "./trading"

describe("journal analytics", () => {
  it("summarises closed trades only, with exact profit factor and hold time", () => {
    const stats = journalStats(trades)
    expect(stats.trades).toBe(2)
    expect(stats.wins).toBe(1)
    expect(stats.losses).toBe(1)
    expect(stats.winRate).toBe(0.5)
    expect(stats.net).toBeCloseTo(167.2)
    expect(stats.profitFactor).toBeCloseTo(268.5 / 101.3)
    expect(stats.averageWin).toBeCloseTo(268.5)
    expect(stats.averageLoss).toBeCloseTo(-101.3)
    expect(stats.best?.id).toBe("1")
    expect(stats.worst?.id).toBe("2")
    expect(stats.contracts).toBe(10)
    expect(stats.averageHoldSeconds).toBe((289 + 9000) / 2)
    expect(journalStats([trades[2]!]).profitFactor).toBe(Infinity)
    expect(journalStats([]).winRate).toBeNull()
  })
  it("groups by the New York close date across the UTC midnight", () => {
    expect(newYorkDate("2026-09-23T03:30:00Z")).toEqual({ date: "2026-09-22", weekday: 2 })
    const days = dailyResults(trades)
    expect([...days.keys()]).toEqual(["2026-09-23", "2026-09-22"])
    expect(days.get("2026-09-22")).toEqual({ date: "2026-09-22", net: 268.5, trades: 1, wins: 1 })
  })
  it("lays out Sunday-first month weeks with padding", () => {
    const weeks = monthWeeks(2026, 9)
    expect(weeks[0]).toEqual([null, null, "2026-09-01", "2026-09-02", "2026-09-03", "2026-09-04", "2026-09-05"])
    expect(weeks.at(-1)).toEqual(["2026-09-27", "2026-09-28", "2026-09-29", "2026-09-30", null, null, null])
    expect(monthWeeks(2026, 2).flat().filter(Boolean)).toHaveLength(28)
  })
  it("buckets by hold time, weekday and month, filtered by calls or puts", () => {
    const duration = tradeBuckets(trades, "duration")
    expect(duration.find((b) => b.label === "1–5m")).toMatchObject({ trades: 1, net: 268.5, winRate: 1 })
    expect(duration.find((b) => b.label === "1–4h")).toMatchObject({ trades: 1, wins: 0, winRate: 0 })
    expect(duration.find((b) => b.label === "< 1m")?.winRate).toBeNull()
    const weekday = tradeBuckets(trades, "weekday")
    expect(weekday.map((b) => b.trades)).toEqual([0, 1, 1, 0, 0])
    expect(tradeBuckets(trades, "month").find((b) => b.label === "Sep")?.trades).toBe(2)
    expect(tradeBuckets(trades, "month", "put").every((b) => b.trades === 0)).toBe(true)
  })
  it("formats durations and contract labels", () => {
    expect(formatDuration(45)).toBe("45s")
    expect(formatDuration(290)).toBe("4m 50s")
    expect(formatDuration(7500)).toBe("2h 5m")
    expect(formatDuration(273_600)).toBe("3d 4h")
    expect(formatDuration(null)).toBe("—")
    expect(contractLabel(trades[2]!)).toBe("SPX Oct 16 7000C")
    expect(parseOsi("SPXW  261022P05000500")).toEqual({ root: "SPXW", expiry: "2026-10-22", type: "put", strike: 5000.5 })
    expect(parseOsi("bad")).toBeNull()
    expect(osiLabel("SPXW  261022P05000000", "SPX")).toBe("SPX Oct 22 5000P")
    expect(osiLabel("bad", "SPX")).toBe("bad")
  })
})

describe("ticket logic", () => {
  it("names strategies from the held position", () => {
    expect(strategyName("buy", "call", 0, 1)).toBe("Long Call")
    expect(strategyName("sell", "put", 0, 2)).toBe("Short Put")
    expect(strategyName("sell", "call", 3, 3)).toBe("Close Long Call")
    expect(strategyName("buy", "put", -2, 1)).toBe("Close Short Put")
    expect(strategyName("sell", "call", 2, 5)).toBe("Reverse to Short Call")
    expect(split("sell", 5, 2)).toEqual({ closing: 2, opening: 3 })
    expect(split("buy", 4, 2)).toEqual({ closing: 0, opening: 4 })
  })
  it("mirrors the matcher's far-side marketability", () => {
    expect(marketability("buy", "limit", "4.60", quote)).toMatchObject({ marketable: true, price: 4.6, size: 3 })
    expect(marketability("buy", "limit", "4.55", quote).message).toBe("Rests below the ask ($4.60). Fills when the ask reaches $4.55.")
    expect(marketability("sell", "limit", "4.50", quote).message).toBe("Marketable: fills now at the bid $4.50, up to 10 displayed.")
    expect(marketability("sell", "market", "", quote).message).toBe("Fills now at the bid $4.50, up to 10 displayed; any remainder cancels.")
    expect(marketability("buy", "limit", "", quote).message).toBe("Enter a limit price.")
    expect(marketability("buy", "market", "", { ...quote, ask: null }).marketable).toBe(false)
  })
  it("estimates buying power like the server's reservations", () => {
    const call = { symbol: "SPXW  261022C05000000", underlying: "SPX", expiry: "2026-10-22PM", type: "call" as const, strike: 5000 }
    const long: MarginPosition[] = [{ ...call, quantity: 2, value: 0 }]
    const order = { quantity: 2, price: 4.6, fee: 0.65 }
    expect(orderPowerUse({ ...order, side: "buy" }, call, [], 5000)).toEqual({ effect: expect.closeTo(-921.3, 6), uses: true })
    expect(orderPowerUse({ ...order, side: "sell" }, call, long, 5000)).toEqual({ effect: expect.closeTo(-1.3, 6), uses: false })
    expect(orderPowerUse({ ...order, side: "sell" }, call, [], 5000)!.effect).toBeCloseTo(-200_001.3)
    expect(orderPowerUse({ ...order, side: "buy", quantity: 0 }, call, [], 5000)).toBeNull()
    expect(nakedRequirement("put", 5000, 5200)).toBeCloseTo(84_000)
    expect(nakedRequirement("call", 5200, 5000)).toBeCloseTo(80_000)
    expect(nakedRequirement("put", 5000, null)).toBeCloseTo(100_000)
  })
})

describe("margin", () => {
  const put = (strike: number, quantity: number, value = 0, expiry = "2026-10-22PM") =>
    ({ symbol: `SPXW  ${expiry.slice(2, 4)}${expiry.slice(5, 7)}${expiry.slice(8, 10)}P0${strike}000`, underlying: "SPX", expiry, type: "put" as const, strike, quantity, value })
  const call = (strike: number, quantity: number, value = 0) =>
    ({ symbol: `SPXW  261022C0${strike}000`, underlying: "SPX", expiry: "2026-10-22PM", type: "call" as const, strike, quantity, value })
  it("nets spreads like the server", () => {
    expect(marginRequirement([put(4900, -1, 500)], 5000)).toBeCloseTo(90_500)
    expect(marginRequirement([put(4900, -1, 500), put(4890, 1)], 5000)).toBeCloseTo(1_000)
    expect(marginRequirement([put(4900, 1), put(4890, -1, 400)], 5000)).toBe(0)
    expect(marginRequirement([put(4900, -1, 500), put(4890, 1), call(5100, -1, 300), call(5110, 1)], 5000)).toBeCloseTo(1_000)
    expect(marginRequirement([call(5100, 1), call(5110, -2, 500), call(5120, 1)], 5000)).toBe(0)
    expect(marginRequirement([put(4900, -2, 1000), put(4890, 1)], 5000)).toBeCloseTo(91_500)
    // Across expiries: a later long covers an earlier short, not the reverse.
    expect(marginRequirement([put(4900, -1, 500), put(4900, 1, 0, "2026-10-23PM")], 5000)).toBe(0)
    expect(marginRequirement([put(4900, -1, 500), put(4890, 1, 0, "2026-10-23PM")], 5000)).toBeCloseTo(1_000)
    expect(marginRequirement([put(4890, -1, 400, "2026-10-23PM"), put(4900, 1)], 5000)).toBeCloseTo(400 + 100 * 890)
  })
  it("reserves only a spread's width when legging in, and needs power to uncover a short", () => {
    const short = { symbol: put(4900, 0).symbol, underlying: "SPX", expiry: "2026-10-22PM", type: "put" as const, strike: 4900 }
    const longLeg = { ...short, symbol: put(4890, 0).symbol, strike: 4890 }
    // Selling against a held long: the width less the credit, plus the fee.
    expect(orderPowerUse({ side: "sell", quantity: 1, price: 5.4, fee: 0.65 }, short, [put(4890, 1)], 5000)!.effect).toBeCloseTo(-460.65)
    // Selling the long of a spread leaves the put naked: far more than the sale brings in.
    const spread = [put(4900, -1, 510), put(4890, 1)]
    const uncover = orderPowerUse({ side: "sell", quantity: 1, price: 4, fee: 0.65 }, longLeg, spread, 5000)!
    expect(uncover.uses).toBe(true)
    expect(uncover.effect).toBeCloseTo(-(0.65 + 90_510 - 1_000 - 400))
    // Buying the short back frees more than it costs.
    expect(orderPowerUse({ side: "buy", quantity: 1, price: 5.2, fee: 0.65 }, short, spread, 5000)).toEqual({ effect: expect.closeTo(-0.65, 6), uses: false })
    expect(heldPositions([{ ...portfolio.positions[1]!, mark: null, basis: "-310.00" }])[0]!.value).toBe(310)
  })
})

describe("conditional orders", () => {
  it("places stops against the position and conditional entries by the level's side of spot", () => {
    expect(stopDirection("option", "buy", "call")).toBe("at_or_below")
    expect(stopDirection("option", "sell", "put")).toBe("at_or_above")
    expect(stopDirection("underlying", "buy", "call")).toBe("at_or_below")
    expect(stopDirection("underlying", "buy", "put")).toBe("at_or_above")
    expect(stopDirection("underlying", "sell", "call")).toBe("at_or_above")
    expect(stopDirection("underlying", "sell", "put")).toBe("at_or_below")
    expect(opposite("at_or_below")).toBe("at_or_above")
    expect(crossDirection(5010, 5000)).toBe("at_or_above")
    expect(crossDirection(4990, 5000)).toBe("at_or_below")
    expect(crossDirection(4990, null)).toBe("at_or_above")
    expect(describeTrigger({ source: "option", direction: "at_or_below", level: "3.5" }, "sell", "SPX")).toBe("bid ≤ $3.50")
    expect(describeTrigger({ source: "underlying", direction: "at_or_above", level: "5010" }, "buy", "SPX")).toBe("SPX ≥ 5,010.00")
  })
  it("rounds suggested prices to the root's tier tick", () => {
    expect(roundToTick("SPXW", 6.31)).toBe("6.30")
    expect(roundToTick("SPXW", 2.93)).toBe("2.95")
    expect(roundToTick("SPY", 6.317)).toBe("6.32")
    expect(roundToTick("AAPL", 3.12)).toBe("3.10")
    expect(roundToTick("SPXW", 0.01)).toBe("0.05")
    expect(roundToTick("SPXW", -1)).toBeNull()
  })
})

describe("equity ticks", () => {
  it("steps SPY in pennies, single stocks on penny-pilot tiers and OEX like SPX", () => {
    expect(stepLimitPrice("SPY", "12.30", 1)).toBe("12.31")
    expect(stepLimitPrice("AAPL", "2.99", 1)).toBe("3.00")
    expect(stepLimitPrice("AAPL", "3.00", 1)).toBe("3.05")
    expect(stepLimitPrice("AAPL", "3.00", -1)).toBe("2.99")
    expect(stepLimitPrice("OEX", "3.00", 1)).toBe("3.10")
    expect(stepLimitPrice("VIX", "3.00", 1)).toBe("3.01")
  })
})

describe("money display", () => {
  it("signs P&L with a typographic minus and keeps exact differences", () => {
    expect(signedMoney("267.50")).toBe("+$267.50")
    expect(signedMoney("-10.65")).toBe("−$10.65")
    expect(signedMoney("0.00")).toBe("$0.00")
    expect(signedMoney("0.001")).toBe("$0.00")
    expect(subtractMoney("100267.5", "100000.00")).toBe("267.50")
    expect(subtractMoney("1", "1.000001")).toBe("-0.000001")
    expect(ratio("267.50", "100000")).toBeCloseTo(0.002675)
    expect(ratio("1", "0")).toBeNull()
    expect(signedPercent(0.0112)).toBe("+1.1%")
    expect(signedPercent(-0.011)).toBe("−1.1%")
    expect(signedPercent(0.00001)).toBe("0.0%")
    expect(signedPercent(null)).toBe("—")
  })
})
