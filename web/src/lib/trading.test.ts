import { describe, expect, it } from "vitest"
import { quote } from "../test/trading-fixtures"
import { formatMoney, multiplyMoney, scenarioColour, scenarioScale, sideFromCell, ticketEstimate, validMoney } from "./trading"
import { createTokenStore } from "./write-token"

describe("decimal money", () => {
  it("formats null, zero, signed cents and large values exactly", () => {
    expect(formatMoney(null)).toBe("—")
    expect(formatMoney(undefined)).toBe("—")
    expect(formatMoney("bad")).toBe("—")
    expect(formatMoney("0.00")).toBe("$0.00")
    expect(formatMoney("-0.000")).toBe("$0.00")
    expect(formatMoney("-1250.00")).toBe("−$1,250.00")
    expect(formatMoney("9007199254740993.01")).toBe("$9,007,199,254,740,993.01")
    expect(formatMoney("1.005")).toBe("$1.01")
    expect(formatMoney("-1.005")).toBe("−$1.01")
    expect(formatMoney("0.0049")).toBe("$0.00")
  })
  it("multiplies decimal strings without float rounding", () => {
    expect(multiplyMoney("0.29", 300)).toBe("87.00")
    expect(multiplyMoney("9007199254740993.01", 2)).toBe("18014398509481986.02")
    expect(multiplyMoney("4.60", -200)).toBe("-920.00")
    expect(multiplyMoney(null, 100)).toBeNull()
    expect(multiplyMoney("4.60", 1.2)).toBeNull()
    expect(validMoney("0.00")).toBe(true)
    for (const value of ["", "-1", "1e3", "NaN"]) expect(validMoney(value)).toBe(false)
  })
})
describe("ticket calculations", () => {
  it("estimates premium, fees and the order's own signed Greeks", () => {
    expect(ticketEstimate(quote, "buy", 3, "4.60", "0.65")).toEqual({ premium: "1380.00", fees: "1.95", delta: 150, gamma: .6, vega: 3675, theta: -255 })
    expect(ticketEstimate(quote, "sell", 3, "4.50", "0.65")).toEqual({ premium: "1350.00", fees: "1.95", delta: -150, gamma: -.6, vega: -3675, theta: 255 })
  })
  it("preserves missing analytics, prices and an unknown fee schedule", () => {
    const estimate = ticketEstimate({ ...quote, delta: null, gamma: null, vega: 0, theta: null }, "buy", 2, null, null)
    expect(estimate).toEqual({ premium: null, fees: null, delta: null, gamma: null, vega: 0, theta: null })
    expect(ticketEstimate(quote, "buy", 0, "1", "0").premium).toBeNull()
    expect(ticketEstimate(quote, "buy", 1.5, "1", "0").delta).toBeNull()
  })
  it("maps either option's bid to sell and ask to buy", () => {
    expect(sideFromCell("bid")).toBe("sell")
    expect(sideFromCell("ask")).toBe("buy")
  })
})
describe("scenario colours", () => {
  it("uses the same magnitude on both sides of exact zero", () => {
    const scale = scenarioScale([[null, -2000, 0, 1000]])
    expect(scale).toBe(2000)
    expect(scenarioColour(-1000, scale)).toBe("color-mix(in srgb, var(--bearish) 15%, var(--panel))")
    expect(scenarioColour(1000, scale)).toBe("color-mix(in srgb, var(--bullish) 15%, var(--panel))")
    expect(scenarioColour(0, scale)).toBe("var(--panel)")
    expect(scenarioColour(null, scale)).toBe("var(--panel)")
    expect(scenarioScale([[0, null, NaN]])).toBe(1)
  })
})
describe("write-token storage", () => {
  it("survives denied storage getters and operations in memory", () => {
    for (const storage of [() => { throw new Error("denied") }, () => ({ getItem: () => { throw new Error("denied") }, setItem: () => { throw new Error("denied") }, removeItem: () => { throw new Error("denied") } })]) {
      const store = createTokenStore(storage)
      expect(store.get()).toBe("")
      expect(store.set(" entered-once ")).toBe(false)
      expect(store.get()).toBe("entered-once")
      store.set("")
      expect(store.get()).toBe("")
    }
  })
  it("stores only in the provided session store and supports clear and subscriptions", () => {
    const values = new Map<string, string>()
    const storage = () => ({ getItem: (key: string) => values.get(key) ?? null, setItem: (key: string, value: string) => { values.set(key, value) }, removeItem: (key: string) => { values.delete(key) } })
    const store = createTokenStore(storage)
    let calls = 0
    const unsubscribe = store.subscribe(() => calls++)
    expect(store.set("secret")).toBe(true)
    expect(createTokenStore(storage).get()).toBe("secret")
    store.set("")
    expect(values.size).toBe(0)
    expect(calls).toBe(2)
    unsubscribe()
    store.set("another")
    expect(calls).toBe(2)
  })
})
