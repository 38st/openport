import { useSyncExternalStore } from "react"
import type { DividendPaid, Fill, StockFill } from "../api/trading-types"
import { osiLabel } from "./journal"
import { formatMoney, signedMoney } from "./trading"

/** One-shot alert on an underlying's price reaching a level. */
export interface PriceAlert {
  id: string
  symbol: string
  direction: "above" | "below"
  level: number
  created: string
}
/** Alerts live in this browser and run while the terminal is open. */
export interface AlertSettings {
  /** Notify on each new fill in the account the terminal shows. */
  fills: boolean
  /** Play a short tone with each alert. */
  sound: boolean
  prices: PriceAlert[]
}
const key = "openport.alerts"
export const noAlerts: AlertSettings = { fills: false, sound: true, prices: [] }

/** Stored settings, keeping only well-formed alerts. */
export function parseAlerts(text: string | null): AlertSettings {
  if (!text) return noAlerts
  try {
    const value = JSON.parse(text) as Partial<AlertSettings>
    const prices = Array.isArray(value.prices) ? value.prices.filter((a): a is PriceAlert =>
      a != null && typeof a.id === "string" && typeof a.symbol === "string" && (a.direction === "above" || a.direction === "below") &&
      typeof a.level === "number" && Number.isFinite(a.level) && typeof a.created === "string") : []
    return { fills: value.fills === true, sound: value.sound !== false, prices }
  } catch {
    return noAlerts
  }
}
export function createAlertStore(storage: () => Pick<Storage, "getItem" | "setItem">) {
  let memory: AlertSettings | undefined
  const listeners = new Set<() => void>()
  return {
    get() {
      if (memory !== undefined) return memory
      try { memory = parseAlerts(storage().getItem(key)) } catch { memory = noAlerts }
      return memory
    },
    set(next: AlertSettings) {
      memory = next
      try { storage().setItem(key, JSON.stringify(next)) } catch { /* Keep them for this page. */ }
      listeners.forEach((listener) => listener())
    },
    subscribe(listener: () => void) { listeners.add(listener); return () => { listeners.delete(listener) } },
  }
}
export const alertStore = createAlertStore(() => window.localStorage)
export function useAlertSettings() {
  return useSyncExternalStore(alertStore.subscribe, alertStore.get, () => noAlerts)
}

/** An alert above the price waits for it to rise; one below, for it to fall. */
export function directionFor(level: number, price: number | null | undefined): "above" | "below" {
  return price == null || !Number.isFinite(price) || level >= price ? "above" : "below"
}
export function reached(alert: PriceAlert, price: number | null | undefined): boolean {
  if (price == null || !Number.isFinite(price)) return false
  return alert.direction === "above" ? price >= alert.level : price <= alert.level
}
const level = (value: number) => value.toLocaleString("en-US", { minimumFractionDigits: 2, maximumFractionDigits: 2 })
/** "SPX rises to 5,000.00" */
export function describeAlert(alert: Pick<PriceAlert, "symbol" | "direction" | "level">): string {
  return `${alert.symbol} ${alert.direction === "above" ? "rises to" : "falls to"} ${level(alert.level)}`
}
export function priceAlertMessage(alert: PriceAlert, price: number) {
  return { title: `${alert.symbol} ${alert.direction === "above" ? "rose to" : "fell to"} ${level(alert.level)}`, body: `Now ${level(price)}.` }
}
/** "Bought 2 SPX Oct 22 5000C at $4.20" */
export function fillMessage(fill: Fill) {
  const verb = fill.side === "buy" ? "Bought" : "Sold"
  return { title: "Order filled", body: `${verb} ${fill.quantity} ${osiLabel(fill.symbol, fill.underlying)} at ${formatMoney(fill.price)}` }
}
/** Shares that changed hands without an order: an assignment or an exercise at expiry. */
export const unordered = (fill: StockFill) => fill.source === "assignment" || fill.source === "expiry_exercise"
/** "Assigned SPY Oct 16 600P" / "Bought 200 SPY at $510.00" */
export function deliveryMessage(fill: StockFill) {
  const contract = fill.option ? osiLabel(fill.option, fill.symbol) : `a ${fill.symbol} option`
  return {
    title: fill.source === "assignment" ? `Assigned ${contract}` : `Exercised ${contract} at expiry`,
    body: `${fill.shares > 0 ? "Bought" : "Sold"} ${Math.abs(fill.shares)} ${fill.symbol} at ${formatMoney(fill.price)}`,
  }
}
/** "SPY dividend" / "+$380.00 on 200 shares at $1.90", or "on 100 shares short" when paid. */
export function dividendMessage(dividend: DividendPaid) {
  const shares = dividend.shares < 0 ? `${-dividend.shares} shares short` : `${dividend.shares} shares`
  return { title: `${dividend.symbol} dividend`, body: `${signedMoney(dividend.amount)} on ${shares} at ${formatMoney(dividend.per_share)}` }
}
/** Fills after the newest one already seen, oldest first. */
export function newFills(fills: readonly Fill[], after: number): Fill[] {
  return fills.filter((f) => Number(f.id) > after).sort((a, b) => Number(a.id) - Number(b.id))
}
export function newestFill(fills: readonly Fill[]): number {
  return fills.reduce((max, f) => Math.max(max, Number(f.id) || 0), 0)
}
