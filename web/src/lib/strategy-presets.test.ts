import { afterEach, describe, expect, it, vi } from "vitest"
import { loadStrategyPresets, saveStrategyPresets, type StrategyPreset } from "./strategy-presets"

const presets: StrategyPreset[] = [{ name: "0DTE put spread 10Δ 5 wide", template: { kind: "vertical", type: "put", direction: "credit", target: { mode: "delta", value: 10 }, width: 5 } }]
afterEach(() => vi.unstubAllGlobals())
describe("strategy preset storage", () => {
  it("round-trips named parameters per underlying", () => {
    const values = new Map<string, string>()
    vi.stubGlobal("localStorage", { getItem: (key: string) => values.get(key), setItem: (key: string, value: string) => values.set(key, value) })
    expect(saveStrategyPresets("SPX", presets)).toBe(true)
    expect(loadStrategyPresets("spx")).toEqual(presets)
    expect(loadStrategyPresets("QQQ")).toEqual([])
    expect(saveStrategyPresets("SPX", [{ ...presets[0]!, name: "Updated" }])).toBe(true)
    expect(loadStrategyPresets("SPX")[0]!.name).toBe("Updated")
  })
  it("ignores corrupt JSON, unknown templates and malformed parameters", () => {
    for (const text of ["{", "null", "{}", '[{"name":"Bad","template":{"kind":"unknown"}}]', JSON.stringify([{ ...presets[0], template: { ...presets[0]!.template, width: -1 } }])]) {
      vi.stubGlobal("localStorage", { getItem: () => text })
      expect(loadStrategyPresets("SPX")).toEqual([])
    }
  })
  it("tolerates unavailable and quota-blocked storage", () => {
    vi.stubGlobal("localStorage", { getItem: () => { throw new Error("Blocked") }, setItem: () => { throw new Error("Quota") } })
    expect(loadStrategyPresets("SPX")).toEqual([])
    expect(saveStrategyPresets("SPX", presets)).toBe(false)
  })
})
