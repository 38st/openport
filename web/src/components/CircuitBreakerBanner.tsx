import { useLive } from "../api/live"
import { pct } from "../lib/format"

const referencePrice = new Intl.NumberFormat("en-US", { minimumFractionDigits: 2, maximumFractionDigits: 2 })
const resumeTime = new Intl.DateTimeFormat("en-US", { timeZone: "America/New_York", hourCycle: "h23", hour: "2-digit", minute: "2-digit" })

export function CircuitBreakerBanner() {
  const { circuitBreaker, underlyings } = useLive()
  if (!circuitBreaker?.active) return circuitBreaker?.inactive_reason ? (
    <div role="status" className="border-b border-line px-4 py-2 text-xs text-muted">
      Circuit breaker inactive: waiting for {circuitBreaker.symbol || "the market’s"} previous close.
    </div>
  ) : null
  // A deeper fall can trip another level before the first halt ends.
  const active = circuitBreaker.halts.filter((h) => h.active)
  const halt = active.sort((a, b) => b.level - a.level)[0]
  if (!halt) return null
  const resume = new Date(Math.max(...active.map((h) => new Date(h.end).getTime())))
  // The server knows the session calendar, including early closes and overrides.
  const restOfDay = halt.level === 3 || (active.some((h) => h.level === 0) && underlyings.some((u) =>
    u.paper?.reason === "MARKET_HALTED" && u.paper.message?.endsWith("for the rest of the day")))
  const reference = circuitBreaker.symbol === "SPY" ? "SPY, standing in for the S&P 500," : "the S&P 500"
  return (
    <div role="status" aria-label="Market-wide trading halt" className="border-b border-warn/40 bg-warn/10 px-4 py-2 text-xs tabular">
      {halt.level === 0 ? "Trading is halted market-wide by the scenario." :
        <>Trading is halted market-wide: {reference} fell {pct(1 - halt.price / halt.reference)} from its previous close of {referencePrice.format(halt.reference)} (a level {halt.level} circuit breaker).</>}
      {restOfDay ? " Trading is halted for the rest of the day." : ` Trading resumes at ${resumeTime.format(resume)} ET.`}
    </div>
  )
}
