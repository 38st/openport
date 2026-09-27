import { useQuery } from "@tanstack/react-query"
import { useRef, useState } from "react"
import { api, type ReplaySource, type ReplayStart } from "../api/client"
import { usePlans } from "../api/trading"
import { Dialog } from "../components/Dialog"
import { formatMoney } from "../lib/trading"
import type { View } from "../lib/route"
import { useLive } from "../api/live"
import type { ReplayRecording, ReplayState } from "../api/types"
import { TradingError, WriteAccess } from "../components/TradingControls"
import { Badge, Empty, PageHeader, Panel, Segmented } from "../components/ui"
import { joinList } from "../lib/format"
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
  const writeStatus = useQuery({ queryKey: ["replay-listing"], queryFn: ({ signal }) => api.replay(signal), staleTime: 5_000 })
  const trading = status?.trading
  const token = useWriteToken()
  const [error, setError] = useState<unknown>()
  const [pending, setPending] = useState(false)
  const busy = useRef(false)
  const mode = writeStatus.data?.write ?? trading?.write ?? "disabled"
  const blocked = mode === "disabled" || (mode === "token" && !token)
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
    start: (source: ReplaySource, speed: number, then: () => void, options?: ReplayStart) => run(async () => { await api.startReplay(source, speed, mode, options); then() }),
    remove: (id: string, then: () => void) => run(async () => { await api.deleteReplay(id, mode); then() }),
  }
}

function Controls({ replay }: { replay: ReplayState }) {
  const controls = useReplayControls()
  return <div className="space-y-3">
    <div className="flex flex-wrap items-center gap-2">
      <Segmented label="Replay speed" value={replay.speed} onChange={(speed) => void controls.speed(speed)}
        options={replaySpeeds.map((speed) => ({ value: speed, label: speedLabel(speed) }))} />
      <button type="button" className="trade-button" disabled={controls.pending || controls.blocked || replay.finished || replay.fast_forwarding}
        onClick={() => void controls.pause(!replay.paused)}>{replay.paused ? "Resume" : "Pause"}</button>
      <button type="button" className="trade-button" disabled={controls.pending || controls.blocked || replay.finished || replay.fast_forwarding || replay.paused}
        title="Play the next event now, skipping a closed market" onClick={() => void controls.skip()}>Skip gap</button>
      <button type="button" className="trade-button" disabled={controls.pending || controls.blocked} onClick={() => void controls.stop()}>Stop</button>
    </div>
    <TradingError error={controls.error} />
  </div>
}

/** What is playing: the demo's simulated day, or a recording from a provider. */
export function replayTitle(replay: ReplayState) {
  return replay.demo ? `${replay.file}${replay.seed ? ` · seed ${replay.seed}` : ""} · simulated prices, not market data` : `${replay.file} · ${replay.provider} · ${replay.symbols.join(", ")}`
}

/**
 * Replays run a recorded day beside the live feed, with their own paper account and
 * chart: pick a recording, or the demo market's simulated day, set the pace, then
 * trade it from every page as if it were that day.
 */
export function ReplayView({ onNavigate }: { onNavigate?: (view: View) => void }) {
  const live = useLive()
  const listing = useQuery({ queryKey: ["replay-listing"], queryFn: ({ signal }) => api.replay(signal), refetchInterval: 5_000 })
  const controls = useReplayControls()
  const [speed, setSpeed] = useState(10)
  const plans = usePlans()
  const [plan, setPlan] = useState("practice")
  const [startAt, setStartAt] = useState("")
  const [paused, setPaused] = useState(false)
  const [seedMode, setSeedMode] = useState("fresh")
  const [seed, setSeed] = useState("")
  const [deleting, setDeleting] = useState<string>()
  const options = (scenario: boolean): ReplayStart => ({ plan, ...(startAt ? { start_at: startAt } : {}), paused,
    ...(scenario && seedMode !== "fresh" ? { seed: seedMode === "scenario" ? "scenario" : seed } : {}) })
  const badSeed = seedMode === "typed" && (!/^\d{1,20}$/.test(seed) || BigInt(seed || "0") > 18446744073709551615n)
  const replay = (live.source.startsWith("history:") ? null : live.replay) ?? listing.data?.replay ?? null
  const recordings = listing.data?.recordings ?? []
  const demos = listing.data?.demos?.length ? listing.data.demos : listing.data?.demo ? [listing.data.demo] : []
  const started = () => { live.switchSource("replay"); void listing.refetch() }
  return <div className="min-w-0 space-y-4">
    <PageHeader title="Replay" subtitle="Trade a recorded day with its own paper account, at the pace you choose">
      {(demos.length > 0 || recordings.length > 0) && <Segmented label="Starting speed" value={speed} onChange={setSpeed}
        options={[1, 10, 60, 0].map((value) => ({ value, label: speedLabel(value) }))} />}
      {live.status?.trading && <WriteAccess trading={live.status.trading} />}
    </PageHeader>
    <Panel title="Start controls">
      <div className="flex flex-wrap items-end gap-3 text-xs">
        <label>Plan<select aria-label="Replay plan" className="trade-input" value={plan} onChange={(event) => setPlan(event.target.value)}>
          <option value="practice">Practice</option>
          {(plans.data?.plans ?? []).filter((p) => p.id !== "practice" && p.rules.phase !== "funded").map((p) => <option key={p.id} value={p.id}>{p.name}</option>)}
        </select></label>
        <label>Start at (New York)<input aria-label="Start at" type="time" className="trade-input" value={startAt} onChange={(event) => setStartAt(event.target.value)} /></label>
        <label>Scenario seed<select aria-label="Seed mode" className="trade-input" value={seedMode} onChange={(event) => setSeedMode(event.target.value)}>
          <option value="fresh">Fresh seed</option><option value="scenario">Scenario’s seed</option><option value="typed">Type a seed</option>
        </select></label>
        {seedMode === "typed" && <label>Seed<input aria-label="Seed" inputMode="numeric" className="trade-input" value={seed} onChange={(event) => setSeed(event.target.value)} /></label>}
        <label className="flex items-center gap-2"><input type="checkbox" checked={paused} onChange={(event) => setPaused(event.target.checked)} />Pause at start</label>
      </div>
      <p className="mt-2 text-xs text-muted">Leave the time blank for the open. Overnight times from 20:15 belong to the evening before the session date; morning times belong to that date.</p>
      {badSeed && <p className="mt-2 text-xs text-warn">Enter a whole seed from 0 to 18446744073709551615.</p>}
      {controls.pending && <p role="status" className="mt-2 text-xs text-muted">Preparing replay…</p>}
      <TradingError error={controls.error} />
    </Panel>
    {replay ? (
      <Panel title="Now replaying" actions={live.source === "replay"
        ? <button type="button" className="trade-button" onClick={() => live.switchSource("live")}>Back to live</button>
        : <button type="button" className="trade-button border-accent" onClick={() => live.switchSource("replay")}>Trade this replay</button>}>
        <div className="mb-3 flex flex-wrap items-baseline gap-x-4 gap-y-1">
          <span className="text-lg font-medium tabular">{replayClock(replay.time)}</span>
          <span className="text-sm text-muted">{replayTitle(replay)}</span>
          {replay.finished ? <Badge tone="neutral">finished</Badge> : replay.paused ? <Badge tone="warn">paused</Badge> : <Badge tone="positive">{speedLabel(replay.speed)}</Badge>}
        </div>
        {replay.fast_forwarding && <div role="status" className="mb-3 text-sm">Preparing start state… {Math.round((replay.progress ?? 0) * 100)}%<progress className="ml-2" max={1} value={replay.progress ?? 0} aria-label="Fast-forward progress" /></div>}
        <Controls replay={replay} />
        {replay.demo && replay.scenario && replay.seed && <button type="button" className="trade-button mt-2" disabled={controls.pending || controls.blocked}
          onClick={() => void controls.start({ demo: replay.scenario! }, replay.speed, started, { plan: replay.plan, seed: replay.seed!, start_at: replay.start_at || undefined, date: replay.date })}>Replay this seed</button>}
      </Panel>
    ) : null}
    {demos.length > 0 && <Panel title="Demo market">
      <p className="mb-3 text-sm">Simulated trading days in {joinList(demos[0]!.symbols)} options: prices are generated on this server, not
        market data. Each plays like a recording, with its own paper account, so every page works while markets are closed.</p>
      <div className="grid gap-2 md:grid-cols-2 xl:grid-cols-3">
        {demos.map((d) => {
          const playing = replay?.demo && replay.file === `Demo market: ${d.title}`
          return <div key={d.id ?? "demo"} className="flex flex-col gap-2 rounded-md border border-border p-3">
            <div className="flex items-baseline justify-between gap-2">
              <span className="font-medium">{d.title ?? "Demo market"}</span>
              <span className="text-[11px] text-muted">{d.symbols.join(", ")}</span>
            </div>
            <span className="text-[11px] text-muted">simulated · {d.session === "overnight" ? "overnight" : "regular session"}</span>
            {d.goal && <p className="text-xs">Goal: {d.goal}</p>}
            {d.description && <p className="text-xs text-muted">{d.description}</p>}
            <button type="button" className="trade-button mt-auto self-start border-accent text-foreground" disabled={controls.pending || controls.blocked || badSeed}
              aria-label={`${playing ? "Restart" : "Start"} ${d.title ?? "the demo"}`}
              onClick={() => void controls.start({ demo: d.id ?? true }, speed, started, options(true))}>{playing ? "Restart" : "Start"}</button>
          </div>
        })}
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
                <td>{recording.provider ?? "—"}{recording.simulated ? " · simulated" : " · recording"}</td>
                <td>{recording.symbols?.join(", ") ?? "—"}</td>
                <td>{recording.started ? replayClock(recording.started) : "—"}</td>
                <td>{size(recording.bytes)}</td>
                <td className="text-right"><button type="button" className="trade-button" disabled={!!recording.error || controls.pending || controls.blocked}
                  aria-label={`Replay ${recording.file}`}
                  onClick={() => void controls.start({ file: recording.file }, speed, started, options(false))}>
                  {replay?.file === recording.file && !replay.demo ? "Restart" : "Replay"}</button></td>
              </tr>)}
            </tbody>
          </table>
        </div>
      )}
    </Panel>
    <Panel title="Finished runs">
      {!listing.data?.history?.length ? <Empty>No finished runs yet.</Empty> : <div className="space-y-2">
        {listing.data.history.map((run) => <div key={run.id} className="flex flex-wrap items-center gap-3 rounded-md border border-border p-3 text-xs">
          <span className="font-medium">{run.file}</span><Badge tone="neutral">replay</Badge>
          <span>{run.demo ? "simulated" : "recording"}{run.seed ? ` · seed ${run.seed}` : ""}</span>
          <span>{run.date} · {run.start_at || "open"} · {run.plan}</span>
          <Badge tone={run.result === "pass" ? "positive" : run.result === "fail" ? "warn" : "neutral"}>{run.result}</Badge>
          <span>P&amp;L {run.pnl === null ? "—" : formatMoney(run.pnl)}{run.valuation_complete === false ? " (incomplete marks)" : ""}</span>
          {run.error && <span className="text-warn">{run.error}</span>}
          <span className="ml-auto flex gap-2">{(["journal", "dashboard"] as const).map((view) => <button key={view} type="button" className="trade-button" disabled={!!run.error}
            onClick={() => { live.switchSource(`history:${run.id}`); onNavigate?.(view) }}>Open {view}</button>)}
            <button type="button" className="trade-button" disabled={controls.blocked || controls.pending} onClick={() => setDeleting(run.id)}>Delete</button></span>
        </div>)}
      </div>}
    </Panel>
    {deleting && <Dialog title="Delete replay run?" onClose={() => setDeleting(undefined)}>
      <p className="text-sm">This permanently deletes the run’s journal and trades. This cannot be undone.</p>
      <TradingError error={controls.error} />
      <button type="button" className="trade-button" disabled={controls.pending} onClick={() => void controls.remove(deleting, () => { if (live.source === `history:${deleting}`) live.switchSource("live"); setDeleting(undefined); void listing.refetch() })}>Delete run</button>
      <button type="button" className="trade-button" onClick={() => setDeleting(undefined)}>Cancel</button>
    </Dialog>}
    <p className="text-[11px] text-muted">
      A replay runs on the recording's own clock, so sessions, the feed delay and paper-trading rules behave as they did that day.
      Each run starts a separate account on the chosen plan. Its journal is kept when storage and writes are enabled; finished runs open read-only. Your live accounts keep running.
    </p>
  </div>
}
