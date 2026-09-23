import type { CandleInterval, Candles, Chain, ExposureMatrix, ReplayListing, ReplayState, Status, Summary, Surface } from "./types"
import type { Account, AccountsResponse, CancelAllResponse, ClosePositionsResponse, CreateAccountRequest, CreateAccountResponse, FillsResponse, KillResponse, Limits, Money, NewOrder, OrderChange, OrderResponse, OrdersResponse, PlansResponse, Portfolio, ResetRequest, Risk, SettlementResponse, SubmitOrderResponse, TradeNote, TradeNoteResponse, TradesResponse, WriteMode } from "./trading-types"
import { activeAccount, MAIN_ACCOUNT } from "../lib/active-account"
import { dataSource } from "../lib/data-source"
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

/** On the replay every route is the replay's: /api/X becomes /api/replay/X. Replay controls stay put. */
function routed(path: string): string {
  if (dataSource.get() !== "replay" || path === "/api/replay" || path.startsWith("/api/replay?")) return path
  return path.replace(/^\/api\//, "/api/replay/")
}
const get = <T,>(path: string, signal?: AbortSignal) => request<T>(routed(path), { signal, headers: { Accept: "application/json" } })
function write<T>(path: string, method: "POST" | "PUT" | "DELETE", mode: WriteMode, body?: unknown) {
  const headers: Record<string, string> = { Accept: "application/json", "Content-Type": "application/json" }
  if (mode === "disabled") return Promise.reject(new ApiError(403, "Trading writes are disabled by the server.", "WRITE_DISABLED"))
  if (mode === "token") {
    const token = writeToken.get()
    if (!token) return Promise.reject(new ApiError(403, "Enter a write token to continue.", "WRITE_TOKEN_REQUIRED"))
    headers.Authorization = `Bearer ${token}`
  }
  return request<T>(routed(path), { method, headers, ...(body === undefined ? {} : { body: JSON.stringify(body) }) })
}

const underlying = (symbol: string) => `/api/underlyings/${encodeURIComponent(symbol)}`
/** Trading routes act on the active account; the main one needs no parameter. */
function scoped(path: string): string {
  const account = activeAccount.get()
  // A replay has one account of its own.
  if (account === MAIN_ACCOUNT || dataSource.get() === "replay") return path
  return `${path}${path.includes("?") ? "&" : "?"}account=${encodeURIComponent(account)}`
}

export const api = {
  portfolio: (signal?: AbortSignal) => get<Portfolio>(scoped("/api/portfolio"), signal),
  orders: (status: "open" | "all" = "all", signal?: AbortSignal) => get<OrdersResponse>(scoped(`/api/orders?status=${status}`), signal),
  fills: (signal?: AbortSignal) => get<FillsResponse>(scoped("/api/fills"), signal),
  risk: (signal?: AbortSignal) => get<Risk>(scoped("/api/risk"), signal),
  submitOrder: (order: NewOrder, mode: WriteMode) => write<SubmitOrderResponse>(scoped("/api/orders"), "POST", mode, order),
  cancelOrder: (id: string, mode: WriteMode) => write<OrderResponse>(scoped(`/api/orders/${encodeURIComponent(id)}`), "DELETE", mode),
  modifyOrder: (id: string, change: OrderChange, mode: WriteMode) => write<SubmitOrderResponse>(scoped(`/api/orders/${encodeURIComponent(id)}`), "PUT", mode, change),
  cancelAllOrders: (underlying: string | null, mode: WriteMode) => write<CancelAllResponse>(scoped("/api/orders/cancel"), "POST", mode, underlying ? { underlying } : {}),
  closePositions: (underlying: string | null, mode: WriteMode) => write<ClosePositionsResponse>(scoped("/api/positions/close"), "POST", mode, underlying ? { underlying } : {}),
  annotateTrade: (id: string, note: TradeNote, mode: WriteMode) => write<TradeNoteResponse>(scoped(`/api/trades/${encodeURIComponent(id)}/note`), "PUT", mode, note),
  exercise: (symbol: string, quantity: number, mode: WriteMode) => write<Portfolio>(scoped("/api/positions/exercise"), "POST", mode, { symbol, quantity }),
  closeStock: (symbol: string, shares: number | null, mode: WriteMode) =>
    write<Portfolio>(scoped("/api/stocks/close"), "POST", mode, shares == null ? { symbol } : { symbol, shares }),
  updateLimits: (expected_revision: string, limits: Limits, mode: WriteMode) => write<Risk>(scoped("/api/risk/limits"), "PUT", mode, { expected_revision, limits }),
  setKill: (action: "trip" | "reset", reason: string, mode: WriteMode) => write<KillResponse>(scoped("/api/risk/kill"), "POST", mode, { action, reason }),
  settle: (symbol: string, value: Money, mode: WriteMode) => write<SettlementResponse>(scoped("/api/settlements"), "POST", mode, { symbol, value }),
  account: (signal?: AbortSignal) => get<Account>(scoped("/api/account"), signal),
  trades: (status: "open" | "closed" | "all" = "all", attempt: "current" | "all" = "current", signal?: AbortSignal) =>
    get<TradesResponse>(scoped(`/api/trades?status=${status}&attempt=${attempt}`), signal),
  plans: (signal?: AbortSignal) => get<PlansResponse>("/api/plans", signal),
  accounts: (signal?: AbortSignal) => get<AccountsResponse>("/api/accounts", signal),
  createAccount: (request: CreateAccountRequest, mode: WriteMode) => write<CreateAccountResponse>("/api/accounts", "POST", mode, request),
  resetAccount: (request: ResetRequest, mode: WriteMode) => write<Account>(scoped("/api/account/reset"), "POST", mode, request),
  requestPayout: (amount: Money, mode: WriteMode) => write<Account>(scoped("/api/account/payout"), "POST", mode, { amount }),
  status: (signal?: AbortSignal) => get<Status>("/api/status", signal),
  summary: (symbol: string, signal?: AbortSignal) => get<Summary>(`${underlying(symbol)}/summary`, signal),
  chain: (symbol: string, expiry: string, window: number, signal?: AbortSignal) =>
    get<Chain>(`${underlying(symbol)}/chain?expiry=${encodeURIComponent(expiry)}&window=${window}`, signal),
  exposure: (symbol: string, expiries: number, window: number, signal?: AbortSignal) =>
    get<ExposureMatrix>(`${underlying(symbol)}/exposure?expiries=${expiries}&window=${window}`, signal),
  surface: (symbol: string, expiries: number, window: number, signal?: AbortSignal) =>
    get<Surface>(`${underlying(symbol)}/surface?expiries=${expiries}&window=${window}`, signal),
  replay: (signal?: AbortSignal) => get<ReplayListing>("/api/replay", signal),
  startReplay: (file: string, speed: number, mode: WriteMode) => write<{ replay: ReplayState }>("/api/replay", "POST", mode, { file, speed }),
  controlReplay: (change: { speed?: number; paused?: boolean; skip?: boolean }, mode: WriteMode) =>
    write<{ replay: ReplayState }>("/api/replay", "PUT", mode, change),
  stopReplay: (mode: WriteMode) => write<{ replay: null }>("/api/replay", "DELETE", mode),
  candles: (symbol: string, interval: CandleInterval, limit: number, signal?: AbortSignal) =>
    get<Candles>(`${underlying(symbol)}/candles?interval=${interval}&limit=${limit}`, signal),
}
