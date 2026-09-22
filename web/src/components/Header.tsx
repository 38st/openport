import { useLive } from "../api/live"
import { clock, count, fixed, price } from "../lib/format"
import { views, type View } from "../lib/route"
import { Flash } from "./Flash"
import { FeedBadge } from "./ui"

const viewLabels: Record<View, string> = {
  chain: "Chain",
  smile: "Volatility",
  exposure: "Exposure",
  engine: "Engine",
}

export function Header({
  symbol,
  view,
  onSymbol,
  onView,
}: {
  symbol: string | null
  view: View
  onSymbol: (symbol: string) => void
  onView: (view: View) => void
}) {
  const { status, tick, connection } = useLive()
  const underlyings = tick?.underlyings ?? status?.underlyings ?? []
  const current = underlyings.find((u) => u.symbol === symbol)
  const feed = tick?.feed ?? status?.feed
  const provider = status?.provider

  return (
    <header className="border-b border-border bg-panel">
      <div className="flex flex-wrap items-center gap-x-5 gap-y-2 px-4 py-2">
        <a href="#/" className="flex items-center gap-2 font-semibold tracking-tight">
          <img src="/favicon.svg" alt="" className="h-5 w-5" />
          OpenPort
        </a>

        <nav className="flex items-center gap-1" aria-label="Underlyings">
          {underlyings.map((u) => (
            <button
              key={u.symbol}
              onClick={() => onSymbol(u.symbol)}
              className={`rounded-md px-2 py-1 text-sm tabular ${u.symbol === symbol ? "bg-raised text-foreground" : "text-muted hover:text-foreground"}`}
            >
              {u.symbol}
            </button>
          ))}
        </nav>

        {current && (
          <div className="flex items-baseline gap-2">
            <Flash value={current.spot} className="px-1 text-lg tabular">
              {price(current.spot)}
            </Flash>
            <span className="text-[11px] text-muted">as of {clock(current.as_of)}</span>
          </div>
        )}

        <div className="ml-auto flex flex-wrap items-center gap-3 text-[11px] text-muted">
          {provider && (
            <span title={provider.realtime ? "real-time data" : `delayed ${provider.delay_seconds / 60} minutes`}>
              <span className="text-foreground">{provider.name}</span> {provider.realtime ? "real-time" : `${provider.delay_seconds / 60}-min delay`}
            </span>
          )}
          {feed && <FeedBadge state={feed.state} message={feed.message} />}
          {tick && (
            <span className="tabular" title="contracts tracked · events per second · last analytics pass">
              {count(tick.engine.contracts)} contracts · {fixed(tick.engine.analytics_ms, 1)} ms
            </span>
          )}
          {connection !== "open" && <span className="text-warn">reconnecting…</span>}
        </div>
      </div>

      <nav className="flex gap-1 px-3" aria-label="Views">
        {views.map((v, i) => (
          <button
            key={v}
            onClick={() => onView(v)}
            className={`border-b-2 px-3 py-1.5 text-sm transition-colors ${
              v === view ? "border-accent text-foreground" : "border-transparent text-muted hover:text-foreground"
            }`}
            title={`${viewLabels[v]} (${i + 1})`}
          >
            {viewLabels[v]}
          </button>
        ))}
      </nav>
    </header>
  )
}
