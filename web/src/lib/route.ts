import { useCallback, useSyncExternalStore } from "react"

export const views = ["chain", "smile", "exposure", "engine"] as const
export type View = (typeof views)[number]

export interface Route {
  symbol: string | null
  view: View
  expiry: string | null
}

/// "#/SPX/chain/2026-10-05PM" <-> { symbol, view, expiry }. Keeping state in the
/// hash makes every screen linkable and survives a reload.
export function parseRoute(hash: string): Route {
  const [symbol, view, expiry] = hash.replace(/^#\/?/, "").split("/")
  return {
    symbol: symbol ? decodeURIComponent(symbol).toUpperCase() : null,
    view: (views as readonly string[]).includes(view ?? "") ? (view as View) : "chain",
    expiry: expiry ? decodeURIComponent(expiry) : null,
  }
}

export function formatRoute(route: Route): string {
  const parts = [route.symbol ?? "", route.view]
  if (route.expiry) parts.push(route.expiry)
  return `#/${parts.map(encodeURIComponent).join("/")}`
}

function subscribe(callback: () => void) {
  window.addEventListener("hashchange", callback)
  return () => window.removeEventListener("hashchange", callback)
}

export function useRoute(): [Route, (patch: Partial<Route>) => void] {
  const hash = useSyncExternalStore(subscribe, () => window.location.hash)
  const route = parseRoute(hash)
  const navigate = useCallback((patch: Partial<Route>) => {
    const next = { ...parseRoute(window.location.hash), ...patch }
    const target = formatRoute(next)
    if (target !== window.location.hash) window.location.hash = target
  }, [])
  return [route, navigate]
}
