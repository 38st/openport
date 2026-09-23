import { useEffect, useState } from "react"
import type { MarketSession, Num } from "../api/types"
import { marketBadge, snapshotFreshness } from "../lib/freshness"

export function AsOf({ asOf, delaySeconds = 0, market, showBadge = true }: {
  asOf: string | null | undefined
  delaySeconds?: Num
  market?: MarketSession | null
  showBadge?: boolean
}) {
  const [now, setNow] = useState(Date.now)
  useEffect(() => {
    // Closed feeds still age even when neither REST nor the socket emits changes.
    const timer = window.setInterval(() => setNow(Date.now()), 30_000)
    return () => window.clearInterval(timer)
  }, [])
  const { label, stale, ageSeconds } = snapshotFreshness(asOf, delaySeconds, now)
  const badge = showBadge ? marketBadge(market, stale) : null
  return (
    <span className="inline-flex flex-wrap items-center gap-1.5 text-[11px] text-muted">
      <time dateTime={ageSeconds == null ? undefined : asOf ?? undefined}>{label}</time>
      {badge && (
        <span className={`rounded-full border px-1.5 py-0.5 font-medium ${badge.tone === "warn" ? "border-warn text-warn" : "border-border text-muted"}`} title={badge.title}>
          {badge.label}
        </span>
      )}
    </span>
  )
}
