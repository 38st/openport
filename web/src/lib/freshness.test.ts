import { describe, expect, it } from "vitest"
import { marketBadge, snapshotFreshness, timestampET } from "./freshness"

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
    for (const delay of [null, -900, NaN, Infinity]) {
      expect(snapshotFreshness("2026-09-22T19:59:00Z", delay, now).stale).toBe(true)
    }
  })
})

describe("market badge", () => {
  const closed = { open: false, note: "Outside regular trading hours", next_open: "2026-09-23T13:30:00Z" }

  it.each([true, false])("uses a neutral closed badge regardless of stale=%s", (stale) => {
    expect(marketBadge(closed, stale)).toEqual({
      label: "market closed", tone: "neutral",
      title: "Outside regular trading hours · Next open: Wed, Sep 23, 2026, 09:30 ET",
    })
  })

  it("only warns about stale data when a session is known to be open", () => {
    expect(marketBadge({ ...closed, open: true }, true)).toMatchObject({ label: "stale", tone: "warn" })
    expect(marketBadge({ ...closed, open: true }, false)).toBeNull()
    for (const market of [null, undefined]) expect(marketBadge(market, true)).toBeNull()
  })

  it("labels global and curb sessions neutrally until their data is stale", () => {
    for (const name of ["global", "curb"] as const) {
      const session = { name, open: true, note: "Trading now" }
      expect(marketBadge(session, false)).toEqual({ label: name === "global" ? "overnight session" : "curb session",
        tone: "neutral", title: "Trading now" })
      expect(marketBadge(session, true)).toMatchObject({ label: "stale", tone: "warn" })
    }
    expect(marketBadge({ name: "closed", open: false, note: "Closed" }, true))
      .toEqual({ label: "market closed", tone: "neutral", title: "Closed" })
  })

  it("omits unknown or invalid next opens, and handles a missing snapshot", () => {
    for (const next_open of [null, "invalid"]) {
      expect(marketBadge({ ...closed, next_open }, snapshotFreshness(null, 0, now).stale)?.title).toBe(closed.note)
    }
  })

  it("formats next opens and health timestamps in Eastern time across DST", () => {
    expect(timestampET("2026-01-06T14:30:00Z")).toBe("Tue, Jan 6, 2026, 09:30 ET")
    expect(timestampET("2026-09-23T13:30:00Z")).toBe("Wed, Sep 23, 2026, 09:30 ET")
    for (const timestamp of [null, undefined, "", "invalid"]) expect(timestampET(timestamp)).toBe("—")
  })
})
