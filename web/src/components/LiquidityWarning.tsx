import type { OptionQuote } from "../api/types"
import { count } from "../lib/format"
import { liquidity, liquidityDetail } from "../lib/liquidity"

export function LiquidityWarning({ legs, market, modeled = false }: {
  legs: { label: string; quote: OptionQuote | null | undefined; side: "buy" | "sell"; quantity: number }[]
  market: boolean
  modeled?: boolean
}) {
  const warnings = legs.filter((leg) => liquidity(leg.quote).cue !== "good")
  if (!warnings.length && !market) return null
  return <div role="status" className="space-y-1 rounded-md border border-warn/40 px-3 py-2 text-xs text-warn">
    {warnings.map((leg) => <p key={leg.label}>{leg.label}: {liquidity(leg.quote).cue === "thin" ? "Thin liquidity" : "No two-sided liquidity"}. {liquidityDetail(leg.quote)}.</p>)}
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
