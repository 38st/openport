import { useQuery } from "@tanstack/react-query"
import { api } from "../api/client"
import { useLive } from "../api/live"
import { count, fixed, isNum, price } from "../lib/format"
import { matchingPayload } from "../lib/payload"
import { providerLabel } from "../lib/provider"
import { views, type View } from "../lib/route"
import { useTheme } from "../lib/theme"
import { AsOf } from "./AsOf"
import { Flash } from "./Flash"
import { FeedBadge } from "./ui"

const viewLabels: Record<View, string> = {
  chain: "Chain",
  smile: "Volatility",
  exposure: "Exposure",
  engine: "Engine",
  portfolio: "Portfolio",
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
  const { status, tick, connection, version, market, underlyings, trading } = useLive()
  const { theme, toggleTheme } = useTheme()
  const current = underlyings.find((u) => u.symbol === symbol)
  const feed = tick?.feed ?? status?.feed
  const provider = status?.provider
  const engine = tick?.engine ?? status?.engine
  const summary = useQuery({
    queryKey: ["summary", symbol, symbol ? version(symbol) : 0],
    queryFn: ({ signal }) => api.summary(symbol as string, signal),
    enabled: symbol != null && current?.as_of != null,
    placeholderData: (previous) => symbol ? matchingPayload(previous, symbol) : undefined,
  })
  const summaryData = symbol ? matchingPayload(summary.data, symbol) : undefined
  const inferredSpot = summaryData?.version === current?.version && summaryData?.spot_source === "parity" && isNum(current?.spot)
  const dataLabel = provider ? providerLabel(provider, feed?.state) : ""

  return (
    <header className="border-b border-border bg-panel">
      <div className="flex flex-wrap items-center gap-x-5 gap-y-2 px-4 py-2">
        <a href="#/" className="flex items-center gap-2 font-semibold tracking-tight">
          <svg viewBox="0 0 32 32" className="h-5 w-5 text-accent" aria-hidden="true">
            <rect width="32" height="32" rx="7" className="fill-raised" />
            <path d="M6 22 C11 22 12 10 16 10 C20 10 21 22 26 22" fill="none" stroke="currentColor" strokeWidth="2.5" strokeLinecap="round" />
            <circle cx="16" cy="10" r="2.2" fill="currentColor" />
          </svg>
          OpenPort
        </a>

        <nav className="flex min-w-0 flex-wrap items-center gap-1" aria-label="Underlyings">
          {underlyings.map((u) => (
            <button
              key={u.symbol}
              onClick={() => onSymbol(u.symbol)}
              className={`min-w-0 rounded-md px-2 py-1 text-sm tabular [overflow-wrap:anywhere] ${u.symbol === symbol ? "bg-raised text-foreground" : "text-muted hover:text-foreground"}`}
            >
              {u.symbol}
            </button>
          ))}
        </nav>

        {current && (
          <div className="flex min-w-0 flex-wrap items-baseline gap-2">
            <span title={inferredSpot ? "No underlying quote from this provider; spot inferred from put-call parity" : undefined}>
              <Flash key={symbol} value={current.spot} className="px-1 text-lg tabular">
                {inferredSpot ? "≈" : ""}{price(current.spot)}
              </Flash>
            </span>
            <span className="min-w-0 text-[11px] text-muted">as of <AsOf asOf={current.as_of} delaySeconds={provider?.delay_seconds} market={market} session={current.session} /></span>
          </div>
        )}

        <div className="ml-auto flex min-w-0 flex-wrap items-center gap-3 text-[11px] text-muted">
          {provider && (
            <span className="min-w-0 [overflow-wrap:anywhere]" title={`${dataLabel}${provider.realtime_plan_dependent ? " · based on the current feed state" : ""}`}>
              <span className="text-foreground">{provider.name}</span> {dataLabel}
            </span>
          )}
          {feed && <FeedBadge state={feed.state} message={feed.message} />}
          {engine && (
            <span className="tabular" title="contracts tracked · events per second · last analytics pass">
              {count(engine.contracts)} contracts · {fixed(engine.analytics_ms, 1)} ms
            </span>
          )}
          {connection !== "open" && <span className="text-warn">reconnecting…</span>}
          <button
            type="button"
            aria-label={`Switch to ${theme === "dark" ? "light" : "dark"} theme`}
            title={`Switch to ${theme === "dark" ? "light" : "dark"} theme`}
            onClick={toggleTheme}
            className="flex h-8 w-8 items-center justify-center rounded-md border border-border text-muted hover:bg-raised hover:text-foreground"
          >
            <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" strokeWidth="1.7" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
              {theme === "dark" ? (
                <>
                  <circle cx="12" cy="12" r="4" />
                  <path d="M12 2v2m0 16v2M2 12h2m16 0h2M5 5l1.5 1.5m11 11L19 19M5 19l1.5-1.5m11-11L19 5" />
                </>
              ) : <path d="M20.5 13A8.5 8.5 0 0 1 11 3.5 8.5 8.5 0 1 0 20.5 13Z" />}
            </svg>
          </button>
        </div>
      </div>

      <nav className="flex flex-wrap gap-1 px-3" aria-label="Views">
        {views.filter((v) => v !== "portfolio" || trading != null).map((v, i) => (
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
