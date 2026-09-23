import { describe, expect, it } from "vitest"
import { snapshotFreshness } from "./freshness"

const now = Date.parse("2026-09-22T20:14:00Z")

describe("snapshot freshness", () => {
  it("shows the actual date in New York and elapsed age, including delayed feeds", () => {
    expect(snapshotFreshness("2026-09-22T19:59:00Z", 900, now)).toEqual({
      label: "Tue, Sep 22, 2026, 15:59 ET, 15 min old",
      ageSeconds: 900,
      stale: false,
    })
  })

  it("marks stale only beyond the provider delay plus ten minutes", () => {
    const asOf = "2026-09-22T19:49:00Z"
    expect(snapshotFreshness(asOf, 900, now).stale).toBe(false)
    expect(snapshotFreshness(asOf, 900, now + 1000).stale).toBe(true)
    expect(snapshotFreshness("2026-09-22T20:04:00Z", 0, now).stale).toBe(false)
    expect(snapshotFreshness("2026-09-22T20:03:59Z", 0, now).stale).toBe(true)
  })

  it("keeps closed-market data visibly dated and aging", () => {
    const asOf = "2026-09-18T20:00:00Z"
    expect(snapshotFreshness(asOf, 900, now)).toMatchObject({
      label: "Fri, Sep 18, 2026, 16:00 ET, 4d 0h old", stale: true,
    })
    expect(snapshotFreshness("2026-09-22T18:00:00Z", 900, now).label).toContain("2h 14m old")
  })

  it("handles missing, invalid and future timestamps without negative ages", () => {
    for (const asOf of [null, undefined, "", "invalid"]) {
      expect(snapshotFreshness(asOf, 900, now)).toEqual({ label: "date / age unavailable", ageSeconds: null, stale: false })
    }
    expect(snapshotFreshness("2026-09-22T20:15:00Z", 900, now)).toMatchObject({ ageSeconds: 0, stale: false })
    expect(snapshotFreshness("2026-09-22T20:15:00Z", 0, NaN).ageSeconds).toBeNull()
  })

  it("uses standard time in winter and treats an invalid delay as real-time", () => {
    expect(snapshotFreshness("2026-01-06T20:59:00Z", 0, Date.parse("2026-01-06T20:59:30Z")).label)
      .toBe("Tue, Jan 6, 2026, 15:59 ET, <1 min old")
    for (const delay of [-900, NaN, Infinity]) {
      expect(snapshotFreshness("2026-09-22T19:59:00Z", delay, now).stale).toBe(true)
    }
  })
})
