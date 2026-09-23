import { useQueryClient } from "@tanstack/react-query"
import { useCallback, useEffect, useState } from "react"
import { useLive } from "./api/live"
import { useAccount } from "./api/trading"
import type { Summary } from "./api/types"
import { Header } from "./components/Header"
import { Sidebar, viewLabels } from "./components/Sidebar"
import { Empty } from "./components/ui"
import { payoutsVisible } from "./lib/payouts"
import { accountViews, navigableViews, useRoute } from "./lib/route"
import { matchingPayload } from "./lib/payload"
import { defaultExpiry } from "./views/ChainView"
import { DashboardView } from "./views/DashboardView"
import { EngineView } from "./views/EngineView"
import { JournalView } from "./views/JournalView"
import { OrdersView } from "./views/OrdersView"
import { PayoutsView } from "./views/PayoutsView"
import { PositionsView } from "./views/PositionsView"
import { RulesView } from "./views/RulesView"
import { TradeView } from "./views/TradeView"

export function App() {
  const [route, navigate] = useRoute()
  const live = useLive()
  const queryClient = useQueryClient()
  const [menu, setMenu] = useState(false)
  const closeMenu = useCallback(() => setMenu(false), [])
  const symbols = (live.tick?.underlyings ?? live.status?.underlyings ?? []).map((u) => u.symbol)
  const symbol = route.symbol && symbols.includes(route.symbol) ? route.symbol : (symbols[0] ?? null)
  const version = symbol ? live.version(symbol) : 0
  const payouts = payoutsVisible(useAccount().data)
  // Account pages need a paper-trading server; analytics-only servers land on Trade.
  const view = accountViews.includes(route.view) && !live.trading ? "chain"
    : route.view === "payouts" && !payouts ? "dashboard" : route.view
  const pages = navigableViews(live.trading != null, payouts)

  useEffect(() => { document.title = `${viewLabels[view]} · OpenPort` }, [view])

  // Number keys switch between the sidebar's pages; left/right step through expiries on the chain.
  useEffect(() => {
    const onKey = (event: KeyboardEvent) => {
      const target = event.target instanceof Element ? event.target : null
      if (event.defaultPrevented || event.metaKey || event.ctrlKey || event.altKey || target?.closest(
        'input, textarea, select, button, [contenteditable]:not([contenteditable="false"]), [role="radiogroup"], [role="tablist"], [role="textbox"], [role="combobox"], [role="slider"], [role="spinbutton"]',
      )) return
      const next = pages[Number(event.key) - 1]
      if (next) {
        event.preventDefault()
        navigate({ view: next })
        return
      }
      if ((event.key === "ArrowLeft" || event.key === "ArrowRight") && symbol && view === "chain") {
        const summary = matchingPayload(queryClient.getQueryData<Summary>(["summary", symbol, version]), symbol)
        const ids = summary?.expiries.map((e) => e.id) ?? []
        if (ids.length === 0) return
        const current = route.expiry ?? defaultExpiry(summary?.expiries ?? [])
        const at = Math.max(0, ids.indexOf(current ?? ""))
        const step = ids[Math.min(ids.length - 1, Math.max(0, at + (event.key === "ArrowRight" ? 1 : -1)))]
        if (step) navigate({ expiry: step })
        event.preventDefault()
      }
    }
    window.addEventListener("keydown", onKey)
    return () => window.removeEventListener("keydown", onKey)
  }, [navigate, queryClient, route.expiry, view, symbol, version, pages])

  const waiting = live.status?.feed.message || "Connecting to openportd…"
  const content = view === "dashboard" ? <DashboardView />
    : view === "positions" ? <PositionsView />
    : view === "orders" ? <OrdersView />
    : view === "journal" ? <JournalView />
    : view === "rules" ? <RulesView />
    : view === "payouts" ? <PayoutsView />
    : view === "engine" ? <EngineView />
    : !symbol ? (
      <Empty>
        <div className="text-center">
          <div>Waiting for the first snapshot…</div>
          <div className="mt-1 text-xs text-faint">{waiting}</div>
        </div>
      </Empty>
    ) : <TradeView symbol={symbol} view={view} expiry={route.expiry} onNavigate={navigate} />

  return (
    <div className="flex min-h-full">
      <Sidebar view={view} onView={(next) => navigate({ view: next })} open={menu} onClose={closeMenu} />
      <div className="flex min-w-0 flex-1 flex-col">
        <Header symbol={symbol} onSymbol={(s) => navigate({ symbol: s, expiry: null })} onMenu={() => setMenu(true)} />
        <main className="min-w-0 flex-1 p-3 lg:p-5">{content}</main>
        <footer className="border-t border-border px-4 py-2 text-[11px] text-faint">
          OpenPort · open-source options analytics and a paper-trading simulator on your own market data · simulated fills, no order routing · not investment advice ·{" "}
          <a className="underline decoration-dotted hover:text-muted" href="https://github.com/38st/openport">
            source
          </a>
        </footer>
      </div>
    </div>
  )
}
