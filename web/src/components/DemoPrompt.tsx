import { useQuery } from "@tanstack/react-query"
import { api } from "../api/client"
import { useLive } from "../api/live"
import { demoPrompt, idleReason, nothingTrades, useDemoPromptHidden } from "../lib/demo"
import type { View } from "../lib/route"
import { useReplayControls } from "../views/ReplayView"
import { TradingError } from "./TradingControls"

/** The server's demo market, offered while nothing on the live feed can be traded. */
export function useDemoOffer() {
  const live = useLive()
  const idle = live.source === "live" && nothingTrades(live.underlyings)
  const listing = useQuery({ queryKey: ["replay-listing"], queryFn: ({ signal }) => api.replay(signal), enabled: idle, staleTime: 30_000 })
  const controls = useReplayControls()
  return {
    offered: idle && !!listing.data?.demo && !listing.data.replay,
    reason: idle ? idleReason(live.underlyings) : "",
    symbols: listing.data?.demo?.symbols ?? [],
    controls,
    /** Starts it at 10× and trades it; without write access, `blocked` shows where to get it. */
    start(blocked: () => void) {
      if (controls.blocked) blocked()
      else void controls.start({ demo: true }, 10, () => live.switchSource("replay"))
    },
  }
}

/** A line under the header while nothing trades: markets closed or the feed stalled. */
export function DemoPrompt({ onNavigate }: { onNavigate: (view: View) => void }) {
  const hidden = useDemoPromptHidden()
  const offer = useDemoOffer()
  if (hidden || !offer.offered) return null
  return (
    <div role="status" aria-label="Demo market" className="border-b border-border bg-raised/60 px-4 py-1.5 text-xs">
      <div className="flex flex-wrap items-center gap-x-3 gap-y-1">
        <span>{offer.reason}. The demo market plays a simulated day in {offer.symbols.join(" and ")} options that you can trade.</span>
        <span className="ml-auto flex flex-wrap gap-1">
          <button type="button" className="trade-button border-accent !py-0.5 text-foreground" disabled={offer.controls.pending}
            onClick={() => offer.start(() => onNavigate("replay"))}>{offer.controls.pending ? "Starting…" : "Try the demo"}</button>
          <button type="button" className="trade-button !py-0.5" onClick={() => demoPrompt.hide()}>Not now</button>
        </span>
      </div>
      <TradingError error={offer.controls.error} />
    </div>
  )
}
