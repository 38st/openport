import type { PreviewLiquidity } from "../api/trading-types"
import type { OptionQuote, QuoteIssue } from "../api/types"
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

const issues: Record<QuoteIssue, string> = {
  no_quote: "No quote",
  no_bid: "One-sided: no bid",
  no_ask: "One-sided: no ask",
  crossed: "Crossed: the bid is above the ask",
  zero_size: "A side shows no displayed size",
}
/** Why a displayed quote cannot fill paper orders, or null when it can (or an older server does not say). */
export function quoteIssueText(quote: Pick<OptionQuote, "executable" | "quote_issue"> | null | undefined): string | null {
  if (!quote || quote.executable !== false) return null
  return quote.quote_issue ? issues[quote.quote_issue] : "Not executable"
}

/**
 * What a previewed order can take on the quotes it would trade against: each leg's
 * displayed size this account's orders have used, and how much of the order the
 * rest leaves to wait, cancel (IOC) or, with impact, fill at simulated depth.
 */
export function displayedSizeNotes(legs: readonly PreviewLiquidity[], label: (leg: PreviewLiquidity) => string,
  options: { ioc: boolean; impact: boolean }): string[] {
  return legs.flatMap((leg) => {
    const name = label(leg)
    const side = leg.side === "buy" ? "ask" : "bid"
    if (!leg.executable) return [`${name}: ${leg.reason?.message ?? "No executable quote"}; new orders on it are refused until it can fill.`]
    const notes: string[] = []
    if (leg.size_left < leg.displayed)
      notes.push(`${name}: ${leg.size_left} of the ${leg.displayed} displayed at the ${side} left for your paper orders until a new quote.`)
    if (leg.contracts > leg.size_left)
      notes.push(options.impact
        ? `${name}: ${leg.contracts} contracts against ${leg.size_left} left; the rest fills at simulated depth prices, within any limit.`
        : `${name}: only ${leg.size_left} of ${leg.contracts} contracts can fill on this quote; ${options.ioc ? "the rest cancels (IOC)" : "the rest waits for a new quote"}.`)
    return notes
  })
}
