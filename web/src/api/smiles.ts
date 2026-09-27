import { useQuery } from "@tanstack/react-query"
import { api } from "./client"
import { useLive } from "./live"
import { matchingPayload } from "../lib/payload"

/** Fetch the surface prefix containing the order's expiries, including later manual selections. */
export function useSmileSurface(symbol: string, ids: readonly string[], enabled = true) {
  const version = useLive().version(symbol)
  const summary = useQuery({
    queryKey: ["summary", symbol, version],
    queryFn: ({ signal }) => api.summary(symbol, signal),
    enabled,
    placeholderData: (previous) => matchingPayload(previous, symbol),
  })
  const expiries = matchingPayload(summary.data, symbol)?.expiries ?? []
  const count = Math.max(0, ...ids.map((id) => expiries.findIndex((e) => e.id === id) + 1))
  const surface = useQuery({
    queryKey: ["surface", symbol, count, 0, version],
    queryFn: ({ signal }) => api.surface(symbol, count, 0, signal),
    enabled: enabled && count > 0,
    placeholderData: (previous) => matchingPayload(previous, symbol),
  })
  return matchingPayload(surface.data, symbol)
}
