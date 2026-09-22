// Shapes of openportd's JSON API. Numbers the engine could not compute are null.

export type Num = number | null

export type FeedState = "connecting" | "live" | "delayed" | "stale" | "error" | "stopped"

export interface ProviderInfo {
  name: string
  realtime: boolean
  delay_seconds: number
  trades: boolean
  open_interest: boolean
  vendor_greeks: boolean
}

export interface EngineInfo {
  events: number
  events_per_second: Num
  analytics_ms: Num
  contracts: number
  uptime_seconds: number
}

export interface UnderlyingStatus {
  symbol: string
  spot: Num
  as_of: string
  version: number
  expiries: number
  options: number
}

export interface Status {
  provider: ProviderInfo
  feed: { state: FeedState; message: string; updated: string | null }
  underlyings: UnderlyingStatus[]
  engine: EngineInfo
}

export interface Expiry {
  id: string
  expiry: string
  settlement: "AM" | "PM"
  expiry_time: string
  days: Num
  forward: Num
  discount: Num
  rate: Num
  rate_fitted: boolean
  atm_iv: Num
  gex: Num
  vex: Num
  strikes: number
}

export interface ExposureSummary {
  gex: Num
  vex: Num
  gamma_flip: Num
  call_wall: Num
  put_wall: Num
}

export interface Summary {
  symbol: string
  spot: Num
  as_of: string
  version: number
  compute_ms: Num
  exposure: ExposureSummary
  expiries: Expiry[]
}

export interface OptionQuote {
  bid: Num
  ask: Num
  mid: Num
  iv: Num
  bid_iv: Num
  ask_iv: Num
  delta: Num
  gamma: Num
  vega: Num
  theta: Num
  vanna: Num
  oi: number
  vendor_iv: Num
}

export interface ChainRow {
  strike: number
  iv: Num
  gex: Num
  vex: Num
  call: OptionQuote | null
  put: OptionQuote | null
}

export interface Chain {
  symbol: string
  spot: Num
  as_of: string
  version: number
  expiry: Expiry
  strikes: ChainRow[]
}

export interface ExposureMatrix {
  symbol: string
  spot: Num
  as_of: string
  version: number
  strikes: number[]
  expiries: { id: string; expiry: string; days: Num; gex: Num[]; vex: Num[] }[]
  total_gex: Num[]
  exposure: ExposureSummary
}

export interface SmilePoint {
  strike: number
  k: Num
  iv: Num
  bid_iv: Num
  ask_iv: Num
}

export interface Surface {
  symbol: string
  spot: Num
  as_of: string
  version: number
  expiries: { id: string; expiry: string; days: Num; forward: Num; atm_iv: Num; points: SmilePoint[] }[]
}

export interface Tick {
  type: "tick"
  feed: { state: FeedState; message: string }
  underlyings: { symbol: string; spot: Num; as_of: string; version: number }[]
  engine: { events_per_second: Num; analytics_ms: Num; contracts: number }
}
