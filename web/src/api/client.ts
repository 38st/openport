import type { CandleInterval, Candles, Chain, ExposureMatrix, Status, Summary, Surface } from "./types"
import type { Account, FillsResponse, KillResponse, Limits, Money, NewOrder, OrderResponse, OrdersResponse, PlansResponse, Portfolio, ResetRequest, Risk, SettlementResponse, SubmitOrderResponse, TradesResponse, WriteMode } from "./trading-types"
import { writeToken } from "../lib/write-token"

export class ApiError extends Error {
  constructor(
    readonly status: number,
    message: string,
    readonly code: string = "HTTP_ERROR",
    readonly actual: number | null = null,
    readonly limit: number | null = null,
    readonly scope: string | null = null,
  ) {
    super(message)
    this.name = "ApiError"
  }
}

export function mapApiError(status: number, body: unknown, fallback: string): ApiError {
  const error = body && typeof body === "object" && "error" in body ? body.error : null
  if (typeof error === "string") return new ApiError(status, error)
  if (error && typeof error === "object") {
    const e = error as Record<string, unknown>
    return new ApiError(status, typeof e.message === "string" ? e.message : fallback,
      typeof e.code === "string" ? e.code : "HTTP_ERROR",
      typeof e.actual === "number" && Number.isFinite(e.actual) ? e.actual : null,
      typeof e.limit === "number" && Number.isFinite(e.limit) ? e.limit : null,
      typeof e.scope === "string" ? e.scope : null)
  }
  return new ApiError(status, fallback)
}

async function request<T>(path: string, init: RequestInit): Promise<T> {
  const response = await fetch(path, init)
  if (!response.ok) {
    const body: unknown = await response.json().catch(() => null)
    throw mapApiError(response.status, body, `${response.status} ${response.statusText}`)
  }
  return (await response.json()) as T
}

const get = <T,>(path: string, signal?: AbortSignal) => request<T>(path, { signal, headers: { Accept: "application/json" } })
function write<T>(path: string, method: "POST" | "PUT" | "DELETE", mode: WriteMode, body?: unknown) {
  const headers: Record<string, string> = { Accept: "application/json", "Content-Type": "application/json" }
  if (mode === "disabled") return Promise.reject(new ApiError(403, "Trading writes are disabled by the server.", "WRITE_DISABLED"))
  if (mode === "token") {
    const token = writeToken.get()
    if (!token) return Promise.reject(new ApiError(403, "Enter a write token to continue.", "WRITE_TOKEN_REQUIRED"))
    headers.Authorization = `Bearer ${token}`
  }
  return request<T>(path, { method, headers, ...(body === undefined ? {} : { body: JSON.stringify(body) }) })
}

const underlying = (symbol: string) => `/api/underlyings/${encodeURIComponent(symbol)}`

export const api = {
  portfolio: (signal?: AbortSignal) => get<Portfolio>("/api/portfolio", signal),
  orders: (status: "open" | "all" = "all", signal?: AbortSignal) => get<OrdersResponse>(`/api/orders?status=${status}`, signal),
  fills: (signal?: AbortSignal) => get<FillsResponse>("/api/fills", signal),
  risk: (signal?: AbortSignal) => get<Risk>("/api/risk", signal),
  submitOrder: (order: NewOrder, mode: WriteMode) => write<SubmitOrderResponse>("/api/orders", "POST", mode, order),
  cancelOrder: (id: string, mode: WriteMode) => write<OrderResponse>(`/api/orders/${encodeURIComponent(id)}`, "DELETE", mode),
  updateLimits: (expected_revision: string, limits: Limits, mode: WriteMode) => write<Risk>("/api/risk/limits", "PUT", mode, { expected_revision, limits }),
  setKill: (action: "trip" | "reset", reason: string, mode: WriteMode) => write<KillResponse>("/api/risk/kill", "POST", mode, { action, reason }),
  settle: (symbol: string, value: Money, mode: WriteMode) => write<SettlementResponse>("/api/settlements", "POST", mode, { symbol, value }),
  account: (signal?: AbortSignal) => get<Account>("/api/account", signal),
  trades: (status: "open" | "closed" | "all" = "all", attempt: "current" | "all" = "current", signal?: AbortSignal) =>
    get<TradesResponse>(`/api/trades?status=${status}&attempt=${attempt}`, signal),
  plans: (signal?: AbortSignal) => get<PlansResponse>("/api/plans", signal),
  resetAccount: (request: ResetRequest, mode: WriteMode) => write<Account>("/api/account/reset", "POST", mode, request),
  requestPayout: (amount: Money, mode: WriteMode) => write<Account>("/api/account/payout", "POST", mode, { amount }),
  status: (signal?: AbortSignal) => get<Status>("/api/status", signal),
  summary: (symbol: string, signal?: AbortSignal) => get<Summary>(`${underlying(symbol)}/summary`, signal),
  chain: (symbol: string, expiry: string, window: number, signal?: AbortSignal) =>
    get<Chain>(`${underlying(symbol)}/chain?expiry=${encodeURIComponent(expiry)}&window=${window}`, signal),
  exposure: (symbol: string, expiries: number, window: number, signal?: AbortSignal) =>
    get<ExposureMatrix>(`${underlying(symbol)}/exposure?expiries=${expiries}&window=${window}`, signal),
  surface: (symbol: string, expiries: number, window: number, signal?: AbortSignal) =>
    get<Surface>(`${underlying(symbol)}/surface?expiries=${expiries}&window=${window}`, signal),
  candles: (symbol: string, interval: CandleInterval, limit: number, signal?: AbortSignal) =>
    get<Candles>(`${underlying(symbol)}/candles?interval=${interval}&limit=${limit}`, signal),
}
