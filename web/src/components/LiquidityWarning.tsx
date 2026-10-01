import type { PreviewLiquidity } from "../api/trading-types"
import type { OptionQuote } from "../api/types"
import { count } from "../lib/format"
import { osiLabel } from "../lib/journal"
import { displayedSizeNotes, liquidity, liquidityDetail, quoteIssueText } from "../lib/liquidity"

export function LiquidityWarning({ legs, market, modeled = false, impact = false, preview, ioc = market }: {
  legs: { label: string; quote: OptionQuote | null | undefined; side: "buy" | "sell"; quantity: number }[]
  market: boolean
  modeled?: boolean
  /** Simulated depth beyond the displayed size (the account's impact ticks). */
  impact?: boolean
  /** The server preview's legs: the displayed size this account has left on each quote. */
  preview?: readonly PreviewLiquidity[]
  /** An unfilled remainder cancels rather than waits. */
  ioc?: boolean
}) {
  const unexecutable = legs.filter((leg) => quoteIssueText(leg.quote) != null)
  const warnings = legs.filter((leg) => quoteIssueText(leg.quote) == null && liquidity(leg.quote).cue !== "good")
  const label = (item: PreviewLiquidity) => legs.find((leg) => leg.quote?.symbol === item.symbol)?.label ?? osiLabel(item.symbol, "")
  // The chain's own flag already says why a quote cannot fill; the preview adds the account's used size.
  const sizes = displayedSizeNotes((preview ?? []).filter((item) => item.executable || !unexecutable.some((leg) => leg.quote?.symbol === item.symbol)),
    label, { ioc, impact })
  if (!warnings.length && !unexecutable.length && !sizes.length && !market) return null
  return <div role="status" className="space-y-1 rounded-md border border-warn/40 px-3 py-2 text-xs text-warn">
    {unexecutable.map((leg) => <p key={`x-${leg.label}`}>{leg.label}: {quoteIssueText(leg.quote)}. Paper orders cannot fill on this quote; new orders are refused until one can.</p>)}
    {warnings.map((leg) => <p key={leg.label}>{leg.label}: {liquidity(leg.quote).cue === "thin" ? "Thin liquidity" : "No two-sided liquidity"}. {liquidityDetail(leg.quote)}.</p>)}
    {sizes.map((note) => <p key={note}>{note}</p>)}
    {market && <>
      {legs.map((leg) => {
        const side = leg.side === "buy" ? "ask" : "bid"
        return <p key={leg.label}>{leg.label}: {count(leg.quantity)} contracts against displayed {side} size {count(leg.quote?.[`${side}_size`])}.</p>
      })}
      <p>{modeled ? "Displayed quotes and sizes are inputs to the account’s simulated latency and impact model."
        : "Fills are simulated against the displayed quote and size only."}</p>
    </>}
  </div>
}
