import { useSyncExternalStore } from "react"
import type { UnderlyingSnapshot } from "../api/types"

/**
 * No underlying on the live feed takes paper orders now: each market is closed or its
 * feed has stalled. A circuit-breaker halt lasts minutes, so it is not offered the demo.
 */
export function nothingTrades(underlyings: UnderlyingSnapshot[]): boolean {
  if (underlyings.some((u) => u.paper?.reason === "MARKET_HALTED")) return false
  return underlyings.length > 0 && underlyings.every((u) => (u.paper ? !u.paper.accepting : u.session?.open === false))
}

/** Why nothing trades, in a few words. */
export function idleReason(underlyings: UnderlyingSnapshot[]): string {
  const stalled = underlyings.filter((u) => u.paper?.reason === "FEED_STALLED").map((u) => u.symbol)
  const closed = underlyings.filter((u) => (u.paper ? u.paper.reason === "SESSION_CLOSED" : u.session?.open === false))
  if (stalled.length + closed.length < underlyings.length) return "Nothing can be traded right now"
  if (stalled.length === 0) return "Options markets are closed"
  if (closed.length === 0) return "The market data feed has stalled"
  return `Options markets are closed and the ${stalled.join(", ")} feed has stalled`
}

const key = "openport.demo-prompt"
/** Whether the demo offer is put away: for the rest of the browser session once dismissed. */
export function createPromptStore(storage: () => Pick<Storage, "getItem" | "setItem">) {
  let hidden: boolean | undefined
  const listeners = new Set<() => void>()
  return {
    get() {
      if (hidden === undefined) {
        try { hidden = storage().getItem(key) === "later" } catch { hidden = false }
      }
      return hidden
    },
    hide() {
      hidden = true
      try { storage().setItem(key, "later") } catch { /* Hidden for this page. */ }
      listeners.forEach((listener) => listener())
    },
    subscribe(listener: () => void) { listeners.add(listener); return () => { listeners.delete(listener) } },
  }
}
export const demoPrompt = createPromptStore(() => window.sessionStorage)
export function useDemoPromptHidden() {
  return useSyncExternalStore(demoPrompt.subscribe, demoPrompt.get, demoPrompt.get)
}
