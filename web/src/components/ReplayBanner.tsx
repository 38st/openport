import { useLive } from "../api/live"
import { replayClock, speedLabel, useReplayControls } from "../views/ReplayView"
import { TradingError } from "./TradingControls"
import { Badge } from "./ui"

/** Always in view while trading a replay: its clock and pace, and the way back to live. */
export function ReplayBanner() {
  const live = useLive()
  const controls = useReplayControls()
  const replay = live.replay
  if (live.source !== "replay" || !replay) return null
  return (
    <div role="status" aria-label="Replay" className="border-b border-warn/40 bg-warn/10 px-4 py-1.5 text-xs">
      <div className="flex flex-wrap items-center gap-x-3 gap-y-1">
        <Badge tone={replay.demo ? "accent" : "warn"}>{replay.demo ? "Demo" : "Replay"}</Badge>
        <span className="font-medium tabular">{replayClock(replay.time)}</span>
        <span className="text-muted">{replay.demo ? "Simulated prices, not market data" : replay.file}</span>
        <span className="text-muted">{replay.finished ? "finished" : replay.paused ? "paused" : speedLabel(replay.speed)}</span>
        <span className="ml-auto flex flex-wrap gap-1">
          {!replay.finished && <button type="button" className="trade-button !py-0.5" disabled={controls.pending || controls.blocked}
            onClick={() => void controls.pause(!replay.paused)}>{replay.paused ? "Resume" : "Pause"}</button>}
          {!replay.finished && !replay.paused && <button type="button" className="trade-button !py-0.5" disabled={controls.pending || controls.blocked}
            onClick={() => void controls.skip()}>Skip gap</button>}
          <button type="button" className="trade-button !py-0.5" onClick={() => live.switchSource("live")}>Back to live</button>
        </span>
      </div>
      <TradingError error={controls.error} />
    </div>
  )
}
