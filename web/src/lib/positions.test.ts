import { describe, expect, it } from "vitest"
import type { Fill, Order, Position, Trade } from "../api/trading-types"
import type { ChainRow } from "../api/types"
import { order, portfolio, quote, trades } from "../test/trading-fixtures"
import { closingPlan, rollPlan, settlementTime, strategyGroups, tradeGroups } from "./positions"

const base = portfolio.positions[0]!
const greeks = { delta: null, gamma: null, vega: null, theta: null, dollar_delta: -200, dollar_gamma_1pct: null, vega_dollars: 10, theta_dollars: 20 }
function put(strike: number, quantity: number, mark: string): Position {
  return { ...base, symbol: `SPXW  261016P0${strike}000`, type: "put", strike, quantity, mark, greeks: { ...greeks, dollar_delta: quantity * 100 } }
}
function combo(id: string, legs: [number, "buy" | "sell"][], units: number, price: string, at = "2026-09-23T15:00:00Z"): Order {
  return { ...order, id, symbol: null, side: null, status: "filled", quantity: units, filled_quantity: units, remaining_quantity: 0,
    limit_price: price, average_fill_price: price, accepted_at: at,
    legs: legs.map(([strike, side]) => ({ symbol: `SPXW  261016P0${strike}000`, side, ratio: 1 })) }
}

describe("held strategies", () => {
  const short = put(6900, -2, "3.00")
  const long = put(6890, 2, "2.50")

  it("group the positions one multi-leg order opened, with net value and P&L", () => {
    const [spread] = strategyGroups([short, long], [combo("7", [[6900, "sell"], [6890, "buy"]], 2, "-1.20")], null)
    expect(spread).toBeDefined()
    expect(spread!.label).toBe("Bull put spread")
    expect(spread!.title).toBe("SPX Oct 16 6900/6890 P")
    expect(spread!.units).toBe(2)
    expect(spread!.cost).toBe(-1.2)
    expect(spread!.value).toBeCloseTo(-0.5, 9)  // short 3.00, long 2.50 per unit
    expect(spread!.pnl).toBe(140)  // (−0.50 − −1.20) × 2 units × 100
    expect(spread!.greeks.dollar_delta).toBe(0)  // −200 and +200, all of both positions
    expect(spread!.profile).toEqual({ maxProfit: 240, maxLoss: 1760, breakevens: [6898.8] })
    expect(spread!.expires).toBe(Date.parse("2026-10-16T20:00:00Z"))
    // The server's own expiry wins: ETF options such as SPY's trade until 16:15.
    const late = (p: typeof short) => ({ ...p, expiry_time: "2026-10-16T20:15:00Z" })
    const [etf] = strategyGroups([late(short), late(long)], [combo("7", [[6900, "sell"], [6890, "buy"]], 2, "-1.20")], null)
    expect(etf!.expires).toBe(Date.parse("2026-10-16T20:15:00Z"))
  })

  it("keep what is still held together, sharing a contract between strategies", () => {
    const orders = [
      combo("7", [[6900, "sell"], [6890, "buy"]], 1, "-1.20", "2026-09-23T15:00:00Z"),
      combo("8", [[6900, "sell"], [6880, "buy"]], 1, "-1.60", "2026-09-23T15:05:00Z"),
    ]
    const groups = strategyGroups([put(6900, -2, "3.00"), put(6890, 1, "2.50"), put(6880, 1, "2.10")], orders, null)
    expect(groups.map((g) => [g.order.id, g.units])).toEqual([["8", 1], ["7", 1]])
    expect(groups[1]!.greeks.dollar_delta).toBe(-100 + 100)  // half the short's delta, all of the long's
  })

  it("leave partly closed or reversed strategies as single contracts", () => {
    const opened = combo("7", [[6900, "sell"], [6890, "buy"]], 2, "-1.20")
    // One unit of the long leg sold separately: one unit is still a spread.
    expect(strategyGroups([short, put(6890, 1, "2.50")], [opened], null).map((g) => g.units)).toEqual([1])
    // Sides no longer match the order's.
    expect(strategyGroups([put(6900, 2, "3.00"), long], [opened], null)).toEqual([])
    // Orders from an earlier attempt do not count.
    expect(strategyGroups([short, long], [opened], "2026-09-23T16:00:00Z")).toEqual([])
    // Expired positions wait for settlement.
    expect(strategyGroups([{ ...short, awaiting_settlement: true }, long], [opened], null)).toEqual([])
  })

  it("close every leg reversed, and roll up to two legs to a later expiry", () => {
    const [spread] = strategyGroups([short, long], [combo("7", [[6900, "sell"], [6890, "buy"]], 2, "-1.20")], null)
    const close = closingPlan(spread!)
    expect(close.units).toBe(2)
    expect(close.legs.map((l) => `${l.side} ${l.strike}`)).toEqual(["buy 6900", "sell 6890"])
    const row = (strike: number, tradable = true): ChainRow => ({ strike, iv: 0.2, gex: null, vex: null, call: null,
      put: { ...quote, symbol: `SPXW  261023P0${strike}000`, tradable, untradable_reason: tradable ? null : "Too far out" } })
    const roll = rollPlan(spread!, { id: "2026-10-23PM", strikes: [row(6890), row(6900)] })
    expect("legs" in roll && roll.legs.map((l) => `${l.side} ${l.strike} ${l.expiry}`)).toEqual([
      "buy 6900 2026-10-16PM", "sell 6890 2026-10-16PM", "sell 6900 2026-10-23PM", "buy 6890 2026-10-23PM"])
    expect(rollPlan(spread!, { id: "2026-10-23PM", strikes: [row(6900)] })).toEqual({ reason: "Oct 23 lists no 6890 put." })
    expect(rollPlan(spread!, { id: "2026-10-23PM", strikes: [row(6890), row(6900, false)] })).toEqual({ reason: "Too far out" })
    expect(rollPlan(spread!, { id: "2026-10-09PM", strikes: [] })).toEqual({ reason: "Roll to a later expiry." })
  })

  it("settle AM contracts at the 09:30 open and PM ones at the 16:00 close, New York time", () => {
    expect(settlementTime("2026-10-16", "PM")).toBe(Date.parse("2026-10-16T20:00:00Z"))
    expect(settlementTime("2026-12-18", "AM")).toBe(Date.parse("2026-12-18T14:30:00Z"))
    expect(settlementTime("bad", "PM")).toBeNull()
  })
})

describe("strategies in the journal", () => {
  const leg = (id: string, strike: number, direction: "long" | "short", fill: string, status: "open" | "closed", net: string): Trade => ({
    ...trades[0]!, id, symbol: `SPXW  261016P0${strike}000`, strike, type: "put", direction, status, fills: [fill], net,
    closed: status === "closed" ? "2026-09-23T17:00:00Z" : null, unrealised: status === "open" ? "15.00" : null })
  const fill = (id: string, order_id: string): Fill => ({ id, order_id, symbol: "", underlying: "SPX", side: "buy", quantity: 1, price: "1.00", fee: "0.65", quote_time: "", time: "" })

  it("group round trips by the multi-leg order that opened them", () => {
    const spread = combo("7", [[6900, "sell"], [6890, "buy"]], 1, "-1.20")
    const list = [leg("a", 6900, "short", "f1", "closed", "100.00"), leg("b", 6890, "long", "f2", "open", "0"), leg("c", 6800, "long", "f3", "closed", "-40.00")]
    const groups = tradeGroups(list, [fill("f1", "7"), fill("f2", "7"), fill("f3", "9")], [spread, { ...order, id: "9" }])
    expect(groups.map((g) => g.key)).toEqual(["order-7", "trade-c"])
    expect(groups[0]!.label).toBe("Bull put spread · SPX Oct 16 6900/6890 P")
    expect(groups[0]!.status).toBe("open")
    expect(groups[0]!.net).toBe(100)
    expect(groups[0]!.unrealised).toBe(15)
    expect(groups[1]!.order).toBeNull()
    expect(groups[1]!.net).toBe(-40)
  })
})
