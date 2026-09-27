import { describe, expect, it } from "vitest"
import { farTemplateChain as far, templateChain as chain } from "../test/template-fixtures"
import { buildTemplate, templateTag, type StrategyTemplate, type TemplateSetup } from "./strategy"

const vertical: StrategyTemplate = { kind: "vertical", type: "put", direction: "credit", target: { mode: "delta", value: 15 }, width: 5 }
function setup(template: StrategyTemplate, near = chain, later = far): TemplateSetup {
  const result = buildTemplate(template, near, later)
  if ("reason" in result) throw new Error(result.reason)
  return result
}
const legs = (s: TemplateSetup) => s.legs.map((l) => [l.side, l.strike, l.type, l.ratio])
const failure = (t: StrategyTemplate, near = chain, later = far) => {
  const result = buildTemplate(t, near, later)
  expect(result).toHaveProperty("reason")
  return "reason" in result ? result.reason : ""
}

describe("strategy templates on a synthetic chain", () => {
  it.each([
    ["put", "credit", 90, 85], ["put", "debit", 90, 95], ["call", "credit", 110, 115], ["call", "debit", 110, 105],
  ] as const)("selects a %s %s vertical using the short contract's delta", (type, direction, short, long) => {
    const result = setup({ ...vertical, type, direction })
    expect(legs(result)).toEqual([["sell", short, type, 1], ["buy", long, type, 1]])
    expect(result.widths).toEqual([5])
    expect(result.tag).toBe(`${type}-${direction}-15d-5w`)
  })
  it("uses the chain's deltas, not inferred deltas from its IV", () => {
    const changed = { ...chain, strikes: chain.strikes.map((r) => ({ ...r, put: { ...r.put!, delta: r.strike === 95 ? -.15 : r.put!.delta } })) }
    expect(setup(vertical, changed).legs[0]!.strike).toBe(95)
  })
  it("snaps forward offsets and widths to listed strikes and reports actual widths", () => {
    const result = setup({ ...vertical, target: { mode: "points", value: 8 }, width: 6 })
    expect(result.legs.map((l) => l.strike)).toEqual([90, 85])
    expect(result.widths).toEqual([5])
    expect(result.tag).toBe("put-credit-8pt-6w")
    expect(setup({ ...vertical, target: { mode: "points", value: -8 } }).legs[0]!.strike).toBe(110)
  })
  it("uses the expiry's expected move for verticals and condors", () => {
    const result = setup({ ...vertical, target: { mode: "moves", value: 1 } })
    expect(result.legs.map((l) => l.strike)).toEqual([90, 85]) // 100 × .2 × √.2 = 8.94 points
    const condor = setup({ kind: "condor", target: { mode: "moves", value: 1 }, width: 5 })
    expect(legs(condor)).toEqual([["buy", 85, "put", 1], ["sell", 90, "put", 1], ["sell", 110, "call", 1], ["buy", 115, "call", 1]])
    expect(condor.widths).toEqual([5, 5])
  })
  it("picks condor shorts by delta and an iron butterfly at the forward", () => {
    expect(setup({ kind: "condor", target: { mode: "delta", value: 15 }, width: 5 }).legs.map((l) => l.strike)).toEqual([85, 90, 110, 115])
    expect(legs(setup({ kind: "iron-butterfly", width: 5 }))).toEqual([["buy", 95, "put", 1], ["sell", 100, "put", 1], ["sell", 100, "call", 1], ["buy", 105, "call", 1]])
  })
  it.each(["buy", "sell"] as const)("picks %s strangles and straddles", (side) => {
    expect(legs(setup({ kind: "strangle", side, delta: 15 }))).toEqual([[side, 90, "put", 1], [side, 110, "call", 1]])
    expect(legs(setup({ kind: "straddle", side }))).toEqual([[side, 100, "put", 1], [side, 100, "call", 1]])
  })
  it.each(["call", "put"] as const)("builds 1:2:1 long %s butterflies at ATM or a snapped centre", (type) => {
    expect(legs(setup({ kind: "butterfly", type, target: { mode: "atm" }, width: 5 }))).toEqual([["buy", 95, type, 1], ["sell", 100, type, 2], ["buy", 105, type, 1]])
    expect(setup({ kind: "butterfly", type, target: { mode: "strike", value: 106 }, width: 5 }).legs.map((l) => l.strike)).toEqual([100, 105, 110])
  })
  it.each(["call", "put"] as const)("builds %s calendars and diagonals with the loaded far expiry", (type) => {
    const calendar: StrategyTemplate = { kind: "calendar", type, target: { mode: "atm" }, farExpiry: far.expiry.id, offset: 0 }
    const result = setup(calendar)
    expect(legs(result)).toEqual([["sell", 100, type, 1], ["buy", 100, type, 1]])
    expect(result.legs.map((l) => l.expiry)).toEqual([chain.expiry.id, far.expiry.id])
    expect(result.widths).toEqual([0])
    const diagonal = setup({ ...calendar, kind: "diagonal", offset: -6, target: { mode: "delta", value: 15 } })
    expect(diagonal.legs.map((l) => l.strike)).toEqual(type === "call" ? [110, 105] : [90, 85])
    expect(diagonal.widths).toEqual([5])
  })
  it("fails precisely for missing quotes instead of choosing a nearby contract", () => {
    const missing = { ...chain, strikes: chain.strikes.map((r) => r.strike === 90 ? { ...r, put: null } : r) }
    expect(failure({ ...vertical, target: { mode: "points", value: 10 } }, missing)).toContain("90 put: no valid two-sided quote")
    const crossed = { ...chain, strikes: chain.strikes.map((r) => ({ ...r, put: { ...r.put!, bid: r.put!.ask! + 1 } })) }
    expect(failure(vertical, crossed)).toContain("no valid two-sided quote")
    const blocked = { ...chain, strikes: chain.strikes.map((r) => ({ ...r, put: { ...r.put!, tradable: false, untradable_reason: "STALE_QUOTE" } })) }
    expect(failure(vertical, blocked)).toContain("STALE_QUOTE")
  })
  it("rejects missing wings, overlapping legs, unequal butterfly wings and unavailable targets", () => {
    expect(failure({ ...vertical, width: 25 })).toContain("outside the loaded strike range")
    expect(failure({ ...vertical, width: 1 })).toContain("snaps to the centre strike")
    expect(failure({ ...vertical, width: 0 })).toContain("Width must be greater")
    expect(failure({ ...vertical, target: { mode: "delta", value: .01 } })).toContain("outside the loaded delta range")
    expect(failure({ kind: "strangle", side: "sell", delta: 60 })).toContain("put must be below the call")
    const asymmetric = { ...chain, strikes: chain.strikes.filter((r) => r.strike !== 95) }
    expect(failure({ kind: "butterfly", type: "call", target: { mode: "atm" }, width: 6 }, asymmetric)).toContain("equal butterfly wings")
  })
  it("does not invent deltas, forwards or expected moves", () => {
    expect(failure(vertical, { ...chain, strikes: chain.strikes.map((r) => ({ ...r, put: { ...r.put!, delta: null } })) })).toContain("No put deltas")
    expect(failure({ ...vertical, target: { mode: "moves", value: 1 } }, { ...chain, expiry: { ...chain.expiry, atm_iv: null } })).toContain("Expected move unavailable")
    expect(failure({ kind: "straddle", side: "sell" }, { ...chain, expiry: { ...chain.expiry, forward: null } })).toContain("no forward")
    expect(failure({ ...vertical, target: { mode: "delta", value: NaN } })).toContain("finite strike target")
    expect(failure(vertical, { ...chain, strikes: [] })).toContain("no listed strikes")
  })
  it("requires a later matching far chain and the same listed strike for a calendar", () => {
    const calendar: StrategyTemplate = { kind: "calendar", type: "call", target: { mode: "atm" }, farExpiry: far.expiry.id, offset: 0 }
    expect(buildTemplate(calendar, chain)).toEqual({ reason: `Load the far chain (${far.expiry.id}) before building this template.` })
    expect(failure(calendar, chain, { ...far, symbol: "QQQ" })).toContain("same underlying")
    expect(failure(calendar, chain, { ...far, expiry: { ...far.expiry, expiry_time: chain.expiry.expiry_time } })).toContain("settle after")
    expect(failure(calendar, chain, { ...far, strikes: far.strikes.filter((r) => r.strike !== 100) })).toContain("no listed 100 strike")
    expect(failure({ ...calendar, kind: "diagonal", offset: 1 })).toContain("snaps to the near strike")
    expect(templateTag(calendar)).toBe(`call-calendar-atm-${far.expiry.id}`)
  })
})
