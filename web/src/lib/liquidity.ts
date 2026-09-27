import type { OptionQuote } from "../api/types"
import { count, pct } from "./format"

export const LIQUIDITY = { maxSpread: 0.20, minVolume: 100, minOpenInterest: 500 } as const
export type LiquidityQuote = Pick<OptionQuote, "bid" | "ask" | "oi" | "volume">
const known = (value: number | null | undefined): value is number => value != null && Number.isFinite(value) && value >= 0

/** Spread as a fraction of the quote mid; missing and crossed quotes have no spread. */
export function spreadShare(quote: LiquidityQuote | null | undefined): number | null {
  if (!quote || !known(quote.bid) || !known(quote.ask) || quote.ask < quote.bid || quote.ask <= 0) return null
  return (quote.ask - quote.bid) / ((quote.ask + quote.bid) / 2)
}

export function volumeOiRatio(quote: LiquidityQuote): number | null {
  return known(quote.volume) && known(quote.oi) && quote.oi > 0 ? quote.volume / quote.oi : null
}

export function liquidity(quote: LiquidityQuote | null | undefined) {
  const spread = spreadShare(quote)
  if (!quote || spread == null || !known(quote.bid) || quote.bid <= 0) {
    return { cue: "none", reason: "No valid two-sided quote", spread } as const
  }
  if (spread > LIQUIDITY.maxSpread) return { cue: "thin", reason: `Spread above ${pct(LIQUIDITY.maxSpread, 0)} of mid`, spread } as const
  if (known(quote.volume) && known(quote.oi) && quote.volume < LIQUIDITY.minVolume && quote.oi < LIQUIDITY.minOpenInterest) {
    return { cue: "thin", reason: `Volume below ${LIQUIDITY.minVolume} and OI below ${LIQUIDITY.minOpenInterest}`, spread } as const
  }
  return { cue: "good", reason: `Spread within ${pct(LIQUIDITY.maxSpread, 0)}; no confirmed low volume and OI pair`, spread } as const
}

export function liquidityDetail(quote: LiquidityQuote | null | undefined): string {
  const result = liquidity(quote)
  const volume = known(quote?.volume) ? count(quote.volume) : "unknown"
  const oi = known(quote?.oi) ? count(quote.oi) : "unknown"
  return `${result.reason} · Spread ${result.spread == null ? "unknown" : pct(result.spread, 1)} · Volume ${volume} · OI ${oi}`
}
