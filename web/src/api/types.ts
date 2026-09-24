// Shapes of openportd's JSON API. Numbers the engine could not compute are null.
import type { TradingStatus } from "./trading-types"

export type Num = number | null

export type FeedState = "connecting" | "live" | "delayed" | "stale" | "error" | "stopped"

export interface ProviderInfo {
  name: string
  /** Generated prices, not market data: the demo market. Absent on older servers. */
  simulated?: boolean
  realtime: boolean
  realtime_plan_dependent?: boolean | null
  delay_seconds: number
  poll_interval_seconds?: Num
  trades: boolean
  open_interest: boolean
  vendor_greeks: boolean
}

export interface EngineMetrics {
  events_per_second: Num
  analytics_ms: Num
  contracts: number
  nonstandard_contracts?: Num
  queue_depth?: Num
  coalesced_events?: Num
  dropped_events?: Num
  overloaded?: boolean | null
}

export interface EngineInfo extends EngineMetrics {
  events: number
  uptime_seconds: number
}

export interface UnderlyingSnapshot {
  symbol: string
  spot: Num
  as_of: string | null
  version: number
  session?: TradingSession | null
  paper?: PaperAcceptance | null
  has_tradable_contracts?: boolean
  state?: FeedState | null
  message?: string | null
  last_success?: string | null
  last_error?: string | null
  last_error_time?: string | null
}

export interface UnderlyingStatus extends UnderlyingSnapshot {
  expiries: number
  options: number
}

export interface TradingSession {
  name: "regular" | "curb" | "global" | "closed"
  open: boolean
  note: string
}

export interface PaperAcceptance {
  accepting: boolean
  reason: string | null
  message: string | null
  /** The session new orders enter, by the market-data clock; null before any data. */
  session?: TradingSession["name"] | null
}

export interface MarketSession {
  open: boolean
  note: string
  next_open: string | null
}

/** One paper account, as status and ticks list them. */
export interface AccountBrief { id: string; name: string; trading: TradingStatus }

export interface Status {
  trading?: TradingStatus | null
  /** Every paper account, the main one first; absent on older servers. */
  accounts?: AccountBrief[] | null
  provider: ProviderInfo
  feed: { state: FeedState; message: string; updated: string | null }
  underlyings: UnderlyingStatus[]
  engine: EngineInfo
  market?: MarketSession | null
}

export type SpotSource = "quote" | "parity" | null

export interface Coverage {
  options: Num
  quoted: Num
  priced: Num
  open_interest: Num
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
  rate_source?: "parity" | "term" | "curve" | "assumed" | null
  rate_curve_symbol?: string | null
  deamericanized?: boolean | null
  atm_iv: Num
  gex: Num
  vex: Num
  strikes: number
  style?: "european" | "american" | null
  coverage?: Coverage | null
}

export interface ExposureSummary {
  gex: Num
  vex: Num
  gamma_flip: Num
  call_wall: Num
  put_wall: Num
  oi_coverage?: Num
}

export interface Summary {
  symbol: string
  spot: Num
  spot_source?: SpotSource
  as_of: string | null
  version: number
  compute_ms: Num
  exposure: ExposureSummary
  expiries: Expiry[]
  american_approximation?: boolean | null
  coverage?: Coverage | null
}

export interface OptionQuote {
  // Absent on servers predating paper trading.
  symbol?: string
  bid_size?: Num
  ask_size?: Num
  tradable?: boolean
  untradable_reason?: string | null
  eep?: Num
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
  oi: Num
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
  spot_source?: SpotSource
  as_of: string | null
  version: number
  expiry: Expiry
  strikes: ChainRow[]
}

export interface ExposureMatrix {
  symbol: string
  spot: Num
  spot_source?: SpotSource
  as_of: string | null
  version: number
  strikes: number[]
  expiries: { id: string; expiry: string; days: Num; gex: Num[]; vex: Num[] }[]
  total_gex: Num[]
  exposure: ExposureSummary
}

export interface SviFit {
  a: number
  b: number
  rho: number
  m: number
  sigma: number
  rmse_vol_points: number
  points: number
  status: "ok"
  reason: string | null
  fit_ms: number
  butterfly_min_g: Num
  butterfly_k: Num
  butterfly_ok: boolean
}

export interface SsviFit {
  rho: Num
  eta: Num
  gamma: Num
  rmse_vol_points: Num
  status: "ok" | "too_few_points" | "failed"
  reason: string | null
  monotone_adjusted: boolean
  fit_ms: number
}

export interface SurfaceExpiry {
  id: string
  expiry: string
  days: Num
  forward: Num
  atm_iv: Num
  points: SmilePoint[]
  ssvi_theta?: Num
  ssvi_rmse_vol_points?: Num
  ssvi_reason?: string | null
  ssvi_min_k?: Num
  ssvi_max_k?: Num
  svi?: SviFit | null
  svi_status?: "ok" | "too_few_points" | "failed"
  svi_reason?: string | null
  svi_points?: number
  svi_fit_ms?: number
  svi_years?: Num
  svi_min_k?: Num
  svi_max_k?: Num
}

export interface SmilePoint {
  strike: number
  k: Num
  iv: Num
  bid_iv: Num
  ask_iv: Num
  svi_iv?: Num
  ssvi_iv?: Num
}

export interface Surface {
  symbol: string
  spot: Num
  spot_source?: SpotSource
  as_of: string | null
  version: number
  expiries: SurfaceExpiry[]
  ssvi?: SsviFit
  calendar_violations?: { earlier: string; later: string; k: number; vol_points?: number; tolerance_vol_points?: number }[]
}

/** The replay running beside the live feed: its recording, speed and clock. */
export interface ReplayState {
  file: string
  /** The demo market: a simulated day rather than a recording of real quotes. */
  demo?: boolean
  provider: string
  symbols: string[]
  started: string | null
  delay_seconds: number
  speed: number
  paused: boolean
  finished: boolean
  /** The replay's clock: when the latest event it played was first received. */
  time: string | null
}
export interface ReplayRecording {
  file: string
  bytes: number
  provider?: string
  symbols?: string[]
  started?: string | null
  delay_seconds?: number
  error?: string
}
/** A simulated day the demo market plays, in these symbols. */
export interface ReplayDemo { id?: string; title?: string; description?: string; provider: string; symbols: string[]; started: string }
export interface ReplayListing {
  directory: string
  recordings: ReplayRecording[]
  /** The default demo day, and every one; absent from older servers. */
  demo?: ReplayDemo | null
  demos?: ReplayDemo[]
  replay: ReplayState | null
}

export interface Tick {
  trading?: TradingStatus | null
  accounts?: AccountBrief[] | null
  /** Replay ticks carry the replay's state. */
  replay?: ReplayState
  type: "tick" | "replay_tick"
  feed: { state: FeedState; message: string }
  underlyings: UnderlyingSnapshot[]
  engine: EngineMetrics
  market?: MarketSession | null
}

export type CandleInterval = "1m" | "5m" | "15m" | "30m" | "1h" | "1d"
/** One OHLC bar; t is its start in Unix seconds of market-data time. */
export interface Candle { t: number; o: number; h: number; l: number; c: number }
export interface Candles { symbol: string; interval: CandleInterval; bars: Candle[] }
