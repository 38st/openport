import { describe, expect, it } from "vitest"
import { expiryCoverage, oiCoverage } from "./coverage"

describe("coverage labels", () => {
  it("shows exact counts and warns for either pricing or OI below 90%", () => {
    const coverage = { options: 100, quoted: 100, priced: 90, open_interest: 97 }
    expect(expiryCoverage(coverage)).toEqual({ label: "priced 90/100 · OI 97/100", low: false })
    expect(expiryCoverage({ ...coverage, priced: 89 })?.low).toBe(true)
    expect(expiryCoverage({ ...coverage, open_interest: 89 })?.low).toBe(true)
    expect(expiryCoverage({ ...coverage, options: 10000, priced: 9999, open_interest: 9999 })?.label)
      .toBe("priced 9999/10000 · OI 9999/10000")
  })

  it("keeps missing counts distinct from zero, with no division by zero", () => {
    expect(expiryCoverage(undefined)).toBeNull()
    expect(expiryCoverage(null)).toBeNull()
    expect(expiryCoverage({ options: 0, quoted: 0, priced: 0, open_interest: 0 }))
      .toEqual({ label: "priced 0/0 · OI 0/0", low: false })
    expect(expiryCoverage({ options: 10, quoted: 0, priced: 0, open_interest: null }))
      .toEqual({ label: "priced 0/10 · OI —/10", low: true })
    expect(expiryCoverage({ options: null, quoted: null, priced: null, open_interest: null }))
      .toEqual({ label: "priced —/— · OI —/—", low: false })
  })

  it("formats exposure coverage and applies the unrounded 90% threshold", () => {
    expect(oiCoverage(0.97)).toEqual({ label: "OI coverage 97%", low: false })
    expect(oiCoverage(0.9)).toEqual({ label: "OI coverage 90%", low: false })
    expect(oiCoverage(0.8999).low).toBe(true)
    expect(oiCoverage(0)).toEqual({ label: "OI coverage 0%", low: true })
    expect(oiCoverage(1)).toEqual({ label: "OI coverage 100%", low: false })
    for (const value of [null, undefined, NaN]) {
      expect(oiCoverage(value)).toEqual({ label: "OI coverage —", low: false })
    }
  })
})
