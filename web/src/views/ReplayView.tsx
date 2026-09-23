import { useQuery } from "@tanstack/react-query"
import { useRef, useState } from "react"
import { api, type ReplaySource } from "../api/client"
import { useLive } from "../api/live"
import type { ReplayRecording, ReplayState } from "../api/types"
import { TradingError, WriteAccess, writeBlocked } from "../components/TradingControls"
import { Badge, Empty, PageHeader, Panel, Segmented } from "../components/ui"
import { useWriteToken } from "../lib/write-token"

export const replaySpeeds = [1, 2, 5, 10, 30, 60, 120, 300, 0] as const
export const speedLabel = (speed: number) => (speed === 0 ? "Max" : `${speed}×`)
const clockFormat = new Intl.DateTimeFormat("en-US", { timeZone: "America/New_York", weekday: "short", month: "short", day: "numeric", hour: "2-digit", minute: "2-digit", second: "2-digit", hourCycle: "h23" })
/** The replay's clock in New York time, "Tue, Sep 22, 15:04:31 ET". */
export const replayClock = (time: string | null) => (time && Number.isFinite(Date.parse(time)) ? `${clockFormat.format(Date.parse(time))} ET` : "Not started")
const size = (bytes: number) => bytes >= 1e9 ? `${(bytes / 1e9).toFixed(1)} GB` : bytes >= 1e6 ? `${(bytes / 1e6).toFixed(1)} MB` : `${Math.max(1, Math.round(bytes / 1e3))} KB`

/** Speed, pause, skip and stop for the running replay, from the page or the banner. */
export function useReplayControls() {
  const { status } = useLive()
  const trading = status?.trading
  const token = useWriteToken()
  const [error, setError] = useState<unknown>()
  const [pending, setPending] = useState(false)
  const busy = useRef(false)
  const mode = trading?.write ?? "disabled"
  const blocked = trading ? writeBlocked(trading, token) : true
  async function run(request: () => Promise<unknown>) {
    if (busy.current) return
    busy.current = true
    setPending(true)
    setError(undefined)
    try { await request() } catch (failure) { setError(failure) } finally { busy.current = false; setPending(false) }
  }
  return {
    error, pending, blocked,
    speed: (speed: number) => run(() => api.controlReplay({ speed }, mode)),
    pause: (paused: boolean) => run(() => api.controlReplay({ paused }, mode)),
    skip: () => run(() => api.controlReplay({ skip: true }, mode)),
    stop: () => run(() => api.stopReplay(mode)),
    start: (source: ReplaySource, speed: number, then: () => void) => run(async () => { await api.startReplay(source, speed, mode); then() }),
  }
}

function Controls({ replay }: { replay: ReplayState }) {
  const controls = useReplayControls()
  return <div className="space-y-3">
    <div className="flex flex-wrap items-center gap-2">
      <Segmented label="Replay speed" value={replay.speed} onChange={(speed) => void controls.speed(speed)}
        options={replaySpeeds.map((speed) => ({ value: speed, label: speedLabel(speed) }))} />
      <button type="button" className="trade-button" disabled={controls.pending || controls.blocked || replay.finished}
        onClick={() => void controls.pause(!replay.paused)}>{replay.paused ? "Resume" : "Pause"}</button>
      <button type="button" className="trade-button" disabled={controls.pending || controls.blocked || replay.finished || replay.paused}
        title="Play the next event now, skipping a closed market" onClick={() => void controls.skip()}>Skip gap</button>
      <button type="button" className="trade-button" disabled={controls.pending || controls.blocked} onClick={() => void controls.stop()}>Stop</button>
    </div>
    <TradingError error={controls.error} />
  </div>
}

/** What is playing: the demo's simulated day, or a recording from a provider. */
export function replayTitle(replay: ReplayState) {
  return replay.demo ? "Demo market · simulated prices, not market data" : `${replay.file} · ${replay.provider} · ${replay.symbols.join(", ")}`
}

/**
 * Replays run a recorded day beside the live feed, with their own paper account and
 * chart: pick a recording, or the demo market's simulated day, set the pace, then
 * trade it from every page as if it were that day.
 */
export function ReplayView() {
  const live = useLive()
  const listing = useQuery({ queryKey: ["replay-listing"], queryFn: ({ signal }) => api.replay(signal), refetchInterval: 5_000 })
  const controls = useReplayControls()
  const [speed, setSpeed] = useState(10)
  const replay = live.replay ?? listing.data?.replay ?? null
  const recordings = listing.data?.recordings ?? []
  const demo = listing.data?.demo
  const started = () => { live.switchSource("replay"); void listing.refetch() }
  return <div className="min-w-0 space-y-4">
    <PageHeader title="Replay" subtitle="Trade a recorded day with its own paper account, at the pace you choose">
      {(demo || recordings.length > 0) && <Segmented label="Starting speed" value={speed} onChange={setSpeed}
        options={[1, 10, 60, 0].map((value) => ({ value, label: speedLabel(value) }))} />}
      {live.status?.trading && <WriteAccess trading={live.status.trading} />}
    </PageHeader>
    {replay ? (
      <Panel title="Now replaying" actions={live.source === "replay"
        ? <button type="button" className="trade-button" onClick={() => live.switchSource("live")}>Back to live</button>
        : <button type="button" className="trade-button border-accent" onClick={() => live.switchSource("replay")}>Trade this replay</button>}>
        <div className="mb-3 flex flex-wrap items-baseline gap-x-4 gap-y-1">
          <span className="text-lg font-medium tabular">{replayClock(replay.time)}</span>
          <span className="text-sm text-muted">{replayTitle(replay)}</span>
          {replay.finished ? <Badge tone="neutral">finished</Badge> : replay.paused ? <Badge tone="warn">paused</Badge> : <Badge tone="positive">{speedLabel(replay.speed)}</Badge>}
        </div>
        <Controls replay={replay} />
      </Panel>
    ) : null}
    {demo && <Panel title="Demo market">
      <div className="flex flex-wrap items-center gap-3">
        <p className="min-w-0 flex-1 text-sm">A simulated trading day in {demo.symbols.join(" and ")} options, from the open to the last
          trade at 4:15 pm: prices are generated on this server, not market data. It plays like a recording, with its own paper
          account, so every page works while markets are closed.</p>
        <button type="button" className="trade-button border-accent text-foreground" disabled={controls.pending || controls.blocked}
          onClick={() => void controls.start({ demo: true }, speed, started)}>
          {controls.pending ? "Starting…" : replay?.demo ? "Restart the demo" : "Start the demo"}</button>
      </div>
    </Panel>}
    <Panel title="Recordings">
      <TradingError error={listing.error ?? controls.error} />
      {!listing.data ? <Empty>Loading recordings…</Empty> : recordings.length === 0 ? (
        <div className="space-y-2 text-sm text-muted">
          <p>No recordings in <code className="text-foreground">{listing.data.directory || "the recordings directory"}</code> yet.</p>
          <p>Start openportd with <code className="text-foreground">--record-dir {listing.data.directory || "~/.openport/recordings"}</code> to record each session there;
            every run becomes a day you can replay and trade again.</p>
        </div>
      ) : (
        <div className="max-w-full overflow-x-auto" tabIndex={0} role="region" aria-label="Recordings">
          <table className="w-full text-left text-xs tabular">
            <thead className="text-[11px] uppercase tracking-wide text-muted"><tr>
              {["Recording", "Provider", "Symbols", "Started", "Size", ""].map((h) => <th key={h} scope="col" className="px-2 py-2 font-normal">{h}</th>)}
            </tr></thead>
            <tbody className="[&_td]:px-2 [&_td]:py-2 [&_tr]:border-t [&_tr]:border-border/40">
              {recordings.map((recording: ReplayRecording) => <tr key={recording.file}>
                <td className="font-medium">{recording.file}{recording.error && <div className="text-[10px] text-warn">{recording.error}</div>}</td>
                <td>{recording.provider ?? "—"}</td>
                <td>{recording.symbols?.join(", ") ?? "—"}</td>
                <td>{recording.started ? replayClock(recording.started) : "—"}</td>
                <td>{size(recording.bytes)}</td>
                <td className="text-right"><button type="button" className="trade-button" disabled={!!recording.error || controls.pending || controls.blocked}
                  aria-label={`Replay ${recording.file}`}
                  onClick={() => void controls.start({ file: recording.file }, speed, started)}>
                  {replay?.file === recording.file && !replay.demo ? "Restart" : "Replay"}</button></td>
              </tr>)}
            </tbody>
          </table>
        </div>
      )}
    </Panel>
    <p className="text-[11px] text-muted">
      A replay runs on the recording's own clock, so sessions, the feed delay and paper-trading rules behave as they did that day.
      Its paper account starts fresh on the practice plan each time and lives only while the replay does; your live accounts keep running.
    </p>
  </div>
}
