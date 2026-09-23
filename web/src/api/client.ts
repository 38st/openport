import type { Chain, ExposureMatrix, Status, Summary, Surface } from "./types"

export class ApiError extends Error {
  constructor(
    readonly status: number,
    message: string,
  ) {
    super(message)
  }
}

async function get<T>(path: string, signal?: AbortSignal): Promise<T> {
  const response = await fetch(path, { signal, headers: { Accept: "application/json" } })
  if (!response.ok) {
    const body = (await response.json().catch(() => null)) as { error?: string } | null
    throw new ApiError(response.status, body?.error ?? `${response.status} ${response.statusText}`)
  }
  return (await response.json()) as T
}

const underlying = (symbol: string) => `/api/underlyings/${encodeURIComponent(symbol)}`

export const api = {
  status: (signal?: AbortSignal) => get<Status>("/api/status", signal),
  summary: (symbol: string, signal?: AbortSignal) => get<Summary>(`${underlying(symbol)}/summary`, signal),
  chain: (symbol: string, expiry: string, window: number, signal?: AbortSignal) =>
    get<Chain>(`${underlying(symbol)}/chain?expiry=${encodeURIComponent(expiry)}&window=${window}`, signal),
  exposure: (symbol: string, expiries: number, window: number, signal?: AbortSignal) =>
    get<ExposureMatrix>(`${underlying(symbol)}/exposure?expiries=${expiries}&window=${window}`, signal),
  surface: (symbol: string, expiries: number, window: number, signal?: AbortSignal) =>
    get<Surface>(`${underlying(symbol)}/surface?expiries=${expiries}&window=${window}`, signal),
}
