import { describe, expect, it } from "vitest"
import { extent, linear, niceTicks } from "../charts/scale"
import { flashDirection } from "./flash"
import { days, expiryLabel, price, vol } from "./format"
import { formatRoute, parseRoute } from "./route"

describe("route", () => {
  it("round-trips symbol, view and expiry through the hash", () => {
    const route = { symbol: "SPX", view: "chain" as const, expiry: "2026-10-16AM" }
    expect(parseRoute(formatRoute(route))).toEqual(route)
  })

  it("falls back to the chain view for anything unknown", () => {
    expect(parseRoute("#/spy/nonsense")).toEqual({ symbol: "SPY", view: "chain", expiry: null })
    expect(parseRoute("")).toEqual({ symbol: null, view: "chain", expiry: null })
  })
})

describe("format", () => {
  it("never renders a missing value as zero", () => {
    expect(price(null)).toBe("–")
    expect(vol(Number.NaN)).toBe("–")
  })

  it("shows vol points and sub-dollar prices sensibly", () => {
    expect(vol(0.1575)).toBe("15.75")
    expect(price(0.05)).toBe("0.050")
    expect(price(41.5)).toBe("41.50")
  })

  it("labels expiries and time left", () => {
    expect(expiryLabel("2026-10-16AM")).toBe("Oct 16")
    expect(expiryLabel("2026-10-16AM", true)).toBe("Oct 16 AM")
    expect(days(0.5)).toBe("12.0h")
    expect(days(3.2)).toBe("3.2d")
  })
})

describe("scale", () => {
  it("maps linearly and inverts", () => {
    const x = linear([0, 10], [100, 200])
    expect(x(5)).toBe(150)
    expect(x.invert(150)).toBe(5)
  })

  it("picks round ticks", () => {
    expect(niceTicks(0, 1, 5)).toEqual([0, 0.2, 0.4, 0.6, 0.8, 1])
    expect(niceTicks(7600, 7900, 4)).toEqual([7600, 7700, 7800, 7900])
  })

  it("ignores missing values in extents", () => {
    expect(extent([3, null, 1, Number.NaN, 2])).toEqual([1, 3])
    expect(extent([null])).toBeNull()
  })
})

describe("flash", () => {
  it("only flashes when an existing value moves", () => {
    expect(flashDirection(undefined, 1)).toBeNull()
    expect(flashDirection(1, 1)).toBeNull()
    expect(flashDirection(1, 2)).toBe("up")
    expect(flashDirection(2, 1)).toBe("down")
  })
})
