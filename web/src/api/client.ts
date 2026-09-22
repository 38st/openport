import type { Chain, ExposureMatrix, Status, Summary, Surface } from "./types"

export class ApiError extends Error {
  constructor(
    readonly status: number,
    message: string,
  ) {
    super(message)
  }
}

async function get<T>(path: string): Promise<T> {
  const response = await fetch(path, { headers: { Accept: "application/json" } })
  if (!response.ok) {
    const body = (await response.json().catch(() => null)) as { error?: string } | null
    throw new ApiError(response.status, body?.error ?? `${response.status} ${response.statusText}`)
  }
  return (await response.json()) as T
}

const underlying = (symbol: string) => `/api/underlyings/${encodeURIComponent(symbol)}`

export const api = {
  status: () => get<Status>("/api/status"),
  summary: (symbol: string) => get<Summary>(`${underlying(symbol)}/summary`),
  chain: (symbol: string, expiry: string, window: number) =>
    get<Chain>(`${underlying(symbol)}/chain?expiry=${encodeURIComponent(expiry)}&window=${window}`),
  exposure: (symbol: string, expiries: number, window: number) =>
    get<ExposureMatrix>(`${underlying(symbol)}/exposure?expiries=${expiries}&window=${window}`),
  surface: (symbol: string, expiries: number, window: number) =>
    get<Surface>(`${underlying(symbol)}/surface?expiries=${expiries}&window=${window}`),
}
