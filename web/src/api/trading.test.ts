import { QueryClient, QueryObserver } from "@tanstack/react-query"
import { afterEach, describe, expect, it, vi } from "vitest"
import { api, ApiError, mapApiError } from "./client"
import { liveState } from "./live"
import { sameAccountPlaceholder, tradingQueries } from "./trading"
import { writeToken } from "../lib/write-token"
import { fill, limits, order, portfolio, risk, status, trading } from "../test/trading-fixtures"

const cleanups: (() => void)[] = []
afterEach(() => { cleanups.splice(0).reverse().forEach((fn) => fn()); writeToken.set(""); vi.unstubAllGlobals(); vi.restoreAllMocks() })

describe("trading API", () => {
  it("maps structured and legacy errors without losing zero or null", () => {
    const error = mapApiError(422, { error: { code: "DELTA_LIMIT", message: "Too much delta", actual: 12, limit: 0, scope: "SPX" } }, "fallback")
    expect(error).toBeInstanceOf(ApiError)
    expect(error).toMatchObject({ status: 422, code: "DELTA_LIMIT", message: "Too much delta", actual: 12, limit: 0, scope: "SPX" })
    expect(mapApiError(409, { error: { code: "LIMITS_REVISION", message: "Changed", actual: null, limit: null, scope: null } }, "fallback")).toMatchObject({ actual: null, limit: null, scope: null })
    expect(mapApiError(404, { error: "Unknown symbol" }, "fallback").message).toBe("Unknown symbol")
    expect(mapApiError(503, null, "fallback").message).toBe("fallback")
  })
  it("sends exact JSON money and a bearer token only in token write mode, including bodyless DELETE", async () => {
    const fetcher = vi.fn(async () => new Response(JSON.stringify({ account_version: "18", order, fills: [fill] }), { status: 201 }))
    vi.stubGlobal("fetch", fetcher)
    writeToken.set("fixture-token")
    const request = { client_order_id: "client-1", symbol: order.symbol, side: "buy", type: "limit", quantity: 3, limit_price: "4.60", time_in_force: "day" } as const
    await api.submitOrder(request, "token")
    expect(fetcher).toHaveBeenLastCalledWith("/api/orders", expect.objectContaining({ method: "POST", headers: { Accept: "application/json", "Content-Type": "application/json", Authorization: "Bearer fixture-token" }, body: JSON.stringify(request) }))
    await api.cancelOrder("a/b", "open")
    expect(fetcher).toHaveBeenLastCalledWith("/api/orders/a%2Fb", { method: "DELETE", headers: { Accept: "application/json", "Content-Type": "application/json" } })
    await api.updateLimits("3", limits, "token")
    expect(fetcher).toHaveBeenLastCalledWith("/api/risk/limits", expect.objectContaining({ method: "PUT", body: JSON.stringify({ expected_revision: "3", limits }) }))
    await api.setKill("trip", "Risk review", "token")
    expect(fetcher).toHaveBeenLastCalledWith("/api/risk/kill", expect.objectContaining({ method: "POST", body: JSON.stringify({ action: "trip", reason: "Risk review" }) }))
    await api.settle(order.symbol, "7812.34", "token")
    expect(fetcher).toHaveBeenLastCalledWith("/api/settlements", expect.objectContaining({ body: JSON.stringify({ symbol: order.symbol, value: "7812.34" }) }))
    await api.submitOrder({ client_order_id: "market-id", symbol: order.symbol, side: "sell", type: "market", quantity: 1, time_in_force: "ioc" }, "open")
    expect(fetcher).toHaveBeenLastCalledWith("/api/orders", expect.objectContaining({ body: JSON.stringify({ client_order_id: "market-id", symbol: order.symbol, side: "sell", type: "market", quantity: 1, time_in_force: "ioc" }) }))
  })
  it("blocks disabled writes and missing tokens before fetch; turns rejected HTTP responses into typed errors", async () => {
    const fetcher = vi.fn(async () => new Response(JSON.stringify({ error: { code: "LIMITS_REVISION", message: "Reload limits", actual: null, limit: null, scope: null } }), { status: 409 }))
    vi.stubGlobal("fetch", fetcher)
    await expect(api.setKill("trip", "review", "disabled")).rejects.toMatchObject({ code: "WRITE_DISABLED" })
    await expect(api.setKill("trip", "review", "token")).rejects.toMatchObject({ code: "WRITE_TOKEN_REQUIRED" })
    expect(fetcher).not.toHaveBeenCalled()
    await expect(api.updateLimits("2", limits, "open")).rejects.toMatchObject({ status: 409, code: "LIMITS_REVISION", message: "Reload limits" })
  })
  it("reads all endpoints without a write token and passes abort signals", async () => {
    const fetcher = vi.fn(async () => new Response("{}"))
    vi.stubGlobal("fetch", fetcher)
    const signal = new AbortController().signal
    await Promise.all([api.portfolio(signal), api.orders("open", signal), api.orders(), api.fills(signal), api.risk(signal)])
    expect(fetcher.mock.calls.map((call) => (call as unknown[])[0])).toEqual(["/api/portfolio", "/api/orders?status=open", "/api/orders?status=all", "/api/fills", "/api/risk"])
    expect(fetcher).toHaveBeenCalledWith("/api/portfolio", { signal, headers: { Accept: "application/json" } })
  })
})
describe("account live updates", () => {
  it("requires the status capability and uses connected tick versions, preserving string versions", () => {
    const tick = { type: "tick" as const, feed: status.feed, underlyings: status.underlyings, engine: status.engine, trading: { ...trading, account_version: "9007199254740993", kill_latched: true } }
    expect(liveState({ ...status, trading: undefined }, tick, "open").trading).toBeUndefined()
    expect(liveState(status, tick, "open").trading?.account_version).toBe("9007199254740993")
    expect(liveState(status, tick, "closed").trading?.account_version).toBe("17")
    expect(liveState(status, { ...tick, trading: null }, "open").trading).toBeNull()
  })
  it("refetches all four resources when the account version changes", async () => {
    let version = "17"
    const fetcher = vi.fn(async (url: string) => {
      const payload = url === "/api/portfolio" ? portfolio : url === "/api/risk" ? risk : url === "/api/fills" ? { fills: [fill] } : { orders: [order] }
      return new Response(JSON.stringify({ ...payload, account_version: version }))
    })
    vi.stubGlobal("fetch", fetcher)
    const client = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity, gcTime: Infinity } } })
    cleanups.push(() => client.clear())
    const first = tradingQueries(1, version, true)
    const observer = new QueryObserver(client, first.portfolio)
    cleanups.push(observer.subscribe(() => {}))
    await Promise.all([client.fetchQuery(first.portfolio), client.fetchQuery(first.orders), client.fetchQuery(first.fills), client.fetchQuery(first.risk)])
    expect(fetcher).toHaveBeenCalledTimes(4)
    version = "18"
    const next = tradingQueries(1, version, true)
    observer.setOptions(next.portfolio)
    await Promise.all([client.fetchQuery(next.portfolio), client.fetchQuery(next.orders), client.fetchQuery(next.fills), client.fetchQuery(next.risk)])
    expect(fetcher).toHaveBeenCalledTimes(8)
    expect(observer.getCurrentResult().data?.account_version).toBe("18")
  })
  it("allows placeholders only in the same account connection", () => {
    expect(sameAccountPlaceholder(portfolio, ["trading", 1, "portfolio", "17"], 1)).toBe(portfolio)
    expect(sameAccountPlaceholder(portfolio, ["trading", 1, "portfolio", "17"], 2)).toBeUndefined()
    expect(sameAccountPlaceholder(portfolio, ["summary", 1], 1)).toBeUndefined()
  })
})
