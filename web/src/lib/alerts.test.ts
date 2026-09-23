import { describe, expect, it, vi } from "vitest"
import { fill } from "../test/trading-fixtures"
import { createAlertStore, describeAlert, directionFor, fillMessage, newestFill, newFills, noAlerts, parseAlerts, priceAlertMessage, reached, type PriceAlert } from "./alerts"
import { createToasts } from "./notify"

const alert: PriceAlert = { id: "a", symbol: "SPX", direction: "above", level: 5000, created: "2026-09-23T19:00:00.000Z" }

describe("price alerts", () => {
  it("wait for the price to reach their level from the side they were set on", () => {
    expect(directionFor(5000, 4990)).toBe("above")
    expect(directionFor(4980, 4990)).toBe("below")
    expect(directionFor(5000, null)).toBe("above")
    expect(reached(alert, 4999.99)).toBe(false)
    expect(reached(alert, 5000)).toBe(true)
    expect(reached({ ...alert, direction: "below" }, 5000.01)).toBe(false)
    expect(reached({ ...alert, direction: "below" }, 4999)).toBe(true)
    expect(reached(alert, null)).toBe(false)
    expect(reached(alert, Number.NaN)).toBe(false)
    expect(describeAlert(alert)).toBe("SPX rises to 5,000.00")
    expect(describeAlert({ ...alert, direction: "below", level: 4950.5 })).toBe("SPX falls to 4,950.50")
    expect(priceAlertMessage(alert, 5001.25)).toEqual({ title: "SPX rose to 5,000.00", body: "Now 5,001.25." })
  })

  it("keep only well-formed alerts from storage", () => {
    expect(parseAlerts(null)).toBe(noAlerts)
    expect(parseAlerts("not json")).toBe(noAlerts)
    const parsed = parseAlerts(JSON.stringify({ fills: true, sound: false, prices: [alert, { ...alert, level: "high" }, null, { ...alert, direction: "sideways" }] }))
    expect(parsed).toEqual({ fills: true, sound: false, prices: [alert] })
    expect(parseAlerts("{}")).toEqual({ fills: false, sound: true, prices: [] })
  })

  it("are remembered per browser, and kept for the page when storage fails", () => {
    const items = new Map<string, string>()
    const store = createAlertStore(() => ({ getItem: (k) => items.get(k) ?? null, setItem: (k, v) => { items.set(k, v) } }))
    const listener = vi.fn()
    store.subscribe(listener)
    expect(store.get()).toBe(noAlerts)
    store.set({ fills: true, sound: true, prices: [alert] })
    expect(listener).toHaveBeenCalledTimes(1)
    expect(parseAlerts(items.get("openport.alerts") ?? null).prices).toEqual([alert])
    const broken = createAlertStore(() => { throw new Error("blocked") })
    expect(broken.get()).toBe(noAlerts)
    broken.set({ ...noAlerts, fills: true })
    expect(broken.get().fills).toBe(true)
  })
})

describe("fill alerts", () => {
  it("describe new fills after the newest one seen", () => {
    const fills = [{ ...fill, id: "3" }, { ...fill, id: "12" }, { ...fill, id: "7" }]
    expect(newestFill(fills)).toBe(12)
    expect(newestFill([])).toBe(0)
    expect(newFills(fills, 5).map((f) => f.id)).toEqual(["7", "12"])
    const message = fillMessage({ ...fill, side: "sell", quantity: 2, price: "4.20" })
    expect(message.title).toBe("Order filled")
    expect(message.body).toMatch(/^Sold 2 .+ at \$4\.20$/)
  })
})

describe("toasts", () => {
  it("show the latest four and leave after their time", () => {
    vi.useFakeTimers()
    const list = createToasts(1000)
    for (let i = 1; i <= 5; i++) list.push(`t${i}`, "body")
    expect(list.get().map((t) => t.title)).toEqual(["t2", "t3", "t4", "t5"])
    list.dismiss(list.get()[0]!.id)
    expect(list.get()).toHaveLength(3)
    vi.advanceTimersByTime(1000)
    expect(list.get()).toEqual([])
    vi.useRealTimers()
  })
})
