import { useEffect, useState } from "react"
import { snapshotFreshness } from "../lib/freshness"

export function AsOf({ asOf, delaySeconds = 0 }: { asOf: string; delaySeconds?: number }) {
  const [now, setNow] = useState(Date.now)
  useEffect(() => {
    // Closed feeds still age even when neither REST nor the socket emits changes.
    const timer = window.setInterval(() => setNow(Date.now()), 30_000)
    return () => window.clearInterval(timer)
  }, [])
  const { label, stale, ageSeconds } = snapshotFreshness(asOf, delaySeconds, now)
  return (
    <span className="inline-flex flex-wrap items-center gap-1.5 text-[11px] text-muted">
      <time dateTime={ageSeconds == null ? undefined : asOf}>{label}</time>
      {stale && (
        <span className="rounded-full border border-warn px-1.5 py-0.5 font-medium text-warn" title="Data is older than the provider's stated delay plus 10 minutes">
          stale
        </span>
      )}
    </span>
  )
}
