import { QueryClient, QueryObserver } from "@tanstack/react-query"
import { afterEach, describe, expect, it, vi } from "vitest"
import { connectLive } from "./connection"
import { liveState } from "./live"
import type { Status, Tick } from "./types"

const status: Status = {
  provider: { name: "test", realtime: true, delay_seconds: 0, trades: false, open_interest: true, vendor_greeks: false },
  feed: { state: "live", message: "REST", updated: null },
  underlyings: [{ symbol: "SPX", spot: 7002, as_of: "2026-09-22T20:00:00Z", version: 12, expiries: 2, options: 100 }],
  engine: { events: 100, events_per_second: 2, analytics_ms: 3, contracts: 100, uptime_seconds: 60 },
}
const tick: Tick = {
  type: "tick",
  feed: { state: "delayed", message: "old socket data" },
  underlyings: [{ symbol: "SPX", spot: 7000, as_of: "2026-09-22T19:59:00Z", version: 10 }],
  engine: { events_per_second: 1, analytics_ms: 4, contracts: 90 },
}

describe("live state", () => {
  it.each(["closed", "connecting"] as const)("uses REST for versions, prices, feed and engine when %s", (connection) => {
    const state = liveState(status, tick, connection)
    expect(state.tick).toBeNull()
    expect(state.status).toBe(status)
    expect(state.version("SPX")).toBe(12)
    expect(state.version("missing")).toBe(0)
  })

  it("uses connected ticks, including a reset version of zero, and REST before the first tick", () => {
    expect(liveState(status, tick, "open").version("SPX")).toBe(10)
    expect(liveState(status, null, "open").version("SPX")).toBe(12)
    const restarted = { ...tick, underlyings: [{ ...tick.underlyings[0]!, version: 0 }] }
    expect(liveState(status, restarted, "open").version("SPX")).toBe(0)
    expect(liveState(undefined, null, "connecting").version("SPX")).toBe(0)
  })
})

class Socket {
  static instances: Socket[] = []
  onopen: (() => void) | null = null
  onclose: (() => void) | null = null
  onmessage: ((event: { data: string }) => void) | null = null
  constructor(readonly url: string) { Socket.instances.push(this) }
  open() { this.onopen?.() }
  close() { this.onclose?.() }
  message(value: Tick) { this.onmessage?.({ data: JSON.stringify(value) }) }
}

const cleanups: (() => void)[] = []
afterEach(() => {
  for (const cleanup of cleanups.splice(0).reverse()) cleanup()
  Socket.instances = []
  vi.useRealTimers()
  vi.unstubAllGlobals()
})

describe("socket recovery", () => {
  it("refetches reused versions on reconnect and marks inactive cache entries stale", async () => {
    vi.useFakeTimers()
    vi.stubGlobal("WebSocket", Socket)
    const client = new QueryClient({ defaultOptions: { queries: { staleTime: Infinity, gcTime: Infinity, retry: false } } })
    cleanups.push(() => client.clear())
    const key = ["summary", "SPX", 1]
    const inactiveKey = ["chain", "SPX", "2026-10-16AM", 1]
    client.setQueryData(key, "old server")
    client.setQueryData(inactiveKey, "inactive old server")
    let server = "first connection"
    const queryFn = vi.fn(async () => server)
    const observer = new QueryObserver<string>(client, { queryKey: key, queryFn })
    cleanups.push(observer.subscribe(() => {}))
    const onTick = vi.fn()
    const onConnection = vi.fn()
    cleanups.push(connectLive("ws://localhost/ws", client, onTick, onConnection))
    const first = Socket.instances[0]!
    first.open()
    await vi.waitFor(() => expect(observer.getCurrentResult().data).toBe("first connection"))
    expect(client.getQueryState(inactiveKey)?.isInvalidated).toBe(true)
    first.message(tick)
    expect(onTick).toHaveBeenLastCalledWith(tick)
    first.close()
    expect(onTick).toHaveBeenLastCalledWith(null)
    expect(onConnection).toHaveBeenLastCalledWith("closed")

    server = "restarted server, same version"
    await vi.advanceTimersByTimeAsync(500)
    const second = Socket.instances[1]!
    second.open()
    await vi.waitFor(() => expect(observer.getCurrentResult().data).toBe(server))
    expect(queryFn).toHaveBeenCalledTimes(2)
    expect(onConnection).toHaveBeenLastCalledWith("open")
    first.message(tick)
    first.close()
    expect(onTick).toHaveBeenLastCalledWith(null)
    expect(onConnection).toHaveBeenLastCalledWith("open")
  })

  it("cancels old in-flight requests before invalidating and stops reconnecting after cleanup", async () => {
    vi.useFakeTimers()
    vi.stubGlobal("WebSocket", Socket)
    const client = new QueryClient({ defaultOptions: { queries: { gcTime: Infinity, retry: false } } })
    cleanups.push(() => client.clear())
    let signal: AbortSignal | undefined
    const request = client.fetchQuery({
      queryKey: ["chain", "SPX", 1],
      queryFn: (context) => { signal = context.signal; return new Promise(() => {}) },
    }).catch(() => undefined)
    const onTick = vi.fn()
    const onConnection = vi.fn()
    const cleanup = connectLive("ws://localhost/ws", client, onTick, onConnection)
    cleanups.push(cleanup)
    const socket = Socket.instances[0]!
    socket.open()
    await request
    expect(signal?.aborted).toBe(true)
    socket.close()
    cleanup()
    onTick.mockClear()
    onConnection.mockClear()
    socket.message(tick)
    socket.open()
    await vi.advanceTimersByTimeAsync(10_000)
    expect(Socket.instances).toHaveLength(1)
    expect(onTick).not.toHaveBeenCalled()
    expect(onConnection).not.toHaveBeenCalled()
  })
})
