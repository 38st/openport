import { describe, expect, it } from "vitest"
import { QueryClient, QueryObserver } from "@tanstack/react-query"
import { matchingPayload } from "./payload"

describe("payload identity", () => {
  const data = { symbol: "SPX", expiry: { id: "2026-10-16AM" }, spot: 7000 }

  it("retains only the selected symbol and, for chains, expiry", () => {
    expect(matchingPayload(data, "SPX")).toBe(data)
    expect(matchingPayload(data, "SPX", "2026-10-16AM")).toBe(data)
    expect(matchingPayload(data, "SPY")).toBeUndefined()
    expect(matchingPayload(data, "SPY", "2026-10-16AM")).toBeUndefined()
    expect(matchingPayload(data, "SPX", "2026-10-16PM")).toBeUndefined()
    expect(matchingPayload(data, "SPX", null)).toBeUndefined()
    expect(matchingPayload(undefined, "SPX")).toBeUndefined()
    expect(matchingPayload({ symbol: "SPX" }, "SPX", "2026-10-16AM")).toBeUndefined()
  })

  it("keeps query placeholders for version refreshes but drops them on symbol changes", () => {
    const client = new QueryClient({ defaultOptions: { queries: { staleTime: Infinity, gcTime: Infinity } } })
    const options = (symbol: string, version: number) => ({
      queryKey: ["summary", symbol, version],
      queryFn: () => new Promise<typeof data>(() => {}),
      placeholderData: (previous: typeof data | undefined) => matchingPayload(previous, symbol),
    })
    client.setQueryData(["summary", "SPX", 1], data)
    const observer = new QueryObserver<typeof data>(client, options("SPX", 1))
    const unsubscribe = observer.subscribe(() => {})
    try {
      observer.setOptions(options("SPX", 2))
      expect(observer.getCurrentResult().data).toBe(data)
      expect(observer.getCurrentResult().isPlaceholderData).toBe(true)
      observer.setOptions(options("SPY", 2))
      expect(observer.getCurrentResult().data).toBeUndefined()
      expect(observer.getCurrentResult().isPlaceholderData).toBe(false)
    } finally {
      unsubscribe()
      client.clear()
    }
  })
})
