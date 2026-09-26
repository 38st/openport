import { useLive } from "../api/live"
import { pct } from "../lib/format"

const referencePrice = new Intl.NumberFormat("en-US", { minimumFractionDigits: 2, maximumFractionDigits: 2 })
const resumeTime = new Intl.DateTimeFormat("en-US", { timeZone: "America/New_York", hourCycle: "h23", hour: "2-digit", minute: "2-digit" })

export function CircuitBreakerBanner() {
  const { circuitBreaker } = useLive()
  if (!circuitBreaker?.active) return null
  // A deeper fall can trip another level before the first halt ends.
  const halt = circuitBreaker.halts.filter((h) => h.active).sort((a, b) => b.level - a.level)[0]
  if (!halt) return null
  const reference = circuitBreaker.symbol === "SPY" ? "SPY, standing in for the S&P 500," : "the S&P 500"
  return (
    <div role="status" aria-label="Market-wide trading halt" className="border-b border-warn/40 bg-warn/10 px-4 py-2 text-xs tabular">
      Trading is halted market-wide: {reference} fell {pct(1 - halt.price / halt.reference)} from its previous close of {referencePrice.format(halt.reference)} (a level {halt.level} circuit breaker).
      {halt.level === 3 ? " Trading is halted for the rest of the day." : ` Trading resumes at ${resumeTime.format(new Date(halt.end))} ET.`}
    </div>
  )
}
