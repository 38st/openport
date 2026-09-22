import { useQueryClient } from "@tanstack/react-query"
import { useEffect } from "react"
import { useLive } from "./api/live"
import type { Summary } from "./api/types"
import { Header } from "./components/Header"
import { Empty } from "./components/ui"
import { useRoute, views } from "./lib/route"
import { ChainView, defaultExpiry } from "./views/ChainView"
import { EngineView } from "./views/EngineView"
import { ExposureView } from "./views/ExposureView"
import { SmileView } from "./views/SmileView"

export function App() {
  const [route, navigate] = useRoute()
  const live = useLive()
  const queryClient = useQueryClient()
  const symbols = (live.tick?.underlyings ?? live.status?.underlyings ?? []).map((u) => u.symbol)
  const symbol = route.symbol && symbols.includes(route.symbol) ? route.symbol : (symbols[0] ?? null)

  // 1-4 switch views; left/right step through expiries on the chain.
  useEffect(() => {
    const onKey = (event: KeyboardEvent) => {
      const target = event.target as HTMLElement | null
      if (event.metaKey || event.ctrlKey || event.altKey || target?.closest("input, textarea, select")) return
      const index = Number(event.key) - 1
      const view = views[index]
      if (view) {
        navigate({ view })
        return
      }
      if ((event.key === "ArrowLeft" || event.key === "ArrowRight") && symbol && route.view === "chain") {
        const summary = queryClient.getQueriesData<Summary>({ queryKey: ["summary", symbol] }).at(-1)?.[1]
        const ids = summary?.expiries.map((e) => e.id) ?? []
        if (ids.length === 0) return
        const current = route.expiry ?? defaultExpiry(summary?.expiries ?? [])
        const at = Math.max(0, ids.indexOf(current ?? ""))
        const next = ids[Math.min(ids.length - 1, Math.max(0, at + (event.key === "ArrowRight" ? 1 : -1)))]
        if (next) navigate({ expiry: next })
        event.preventDefault()
      }
    }
    window.addEventListener("keydown", onKey)
    return () => window.removeEventListener("keydown", onKey)
  }, [navigate, queryClient, route.expiry, route.view, symbol])

  const waiting = live.status?.feed.message || "Connecting to openportd…"

  return (
    <div className="flex min-h-full flex-col">
      <Header
        symbol={symbol}
        view={route.view}
        onSymbol={(s) => navigate({ symbol: s, expiry: null })}
        onView={(view) => navigate({ view })}
      />
      <main className="flex-1 p-3">
        {route.view === "engine" ? (
          <EngineView />
        ) : !symbol ? (
          <Empty>
            <div className="text-center">
              <div>Waiting for the first snapshot…</div>
              <div className="mt-1 text-xs text-faint">{waiting}</div>
            </div>
          </Empty>
        ) : route.view === "chain" ? (
          <ChainView symbol={symbol} expiry={route.expiry} onExpiry={(expiry) => navigate({ expiry })} />
        ) : route.view === "smile" ? (
          <SmileView symbol={symbol} />
        ) : (
          <ExposureView symbol={symbol} />
        )}
      </main>
      <footer className="border-t border-border px-4 py-2 text-[11px] text-faint">
        OpenPort · open-source options analytics on your own market data · not investment advice ·{" "}
        <a className="underline decoration-dotted hover:text-muted" href="https://github.com/38st/openport">
          source
        </a>
      </footer>
    </div>
  )
}
