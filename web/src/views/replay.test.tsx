import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import type { ReactNode } from "react"
import { renderToStaticMarkup } from "react-dom/server"
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest"
import { api } from "../api/client"
import { liveState, useLive } from "../api/live"
import type { ReplayListing, ReplayState, Tick } from "../api/types"
import { ReplayBanner } from "../components/ReplayBanner"
import { activeAccount, MAIN_ACCOUNT } from "../lib/active-account"
import { dataSource } from "../lib/data-source"
import { portfolio, status } from "../test/trading-fixtures"
import { replayClock, ReplayView } from "./ReplayView"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
const clients: QueryClient[] = []
function render(node: ReactNode, listing?: ReplayListing) {
  const client = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity, gcTime: Infinity } } })
  clients.push(client)
  if (listing) client.setQueryData(["replay-listing"], listing)
  return renderToStaticMarkup(<QueryClientProvider client={client}>{node}</QueryClientProvider>)
}
const replay: ReplayState = { file: "cboe-2026-09-22T133000Z.oprec", provider: "cboe", symbols: ["SPX", "SPY"], started: "2026-09-22T13:30:00.000Z",
  delay_seconds: 900, speed: 10, paused: false, finished: false, time: "2026-09-22T19:04:31.000Z" }
const recordings: ReplayListing = { directory: "/home/trader/.openport/recordings", replay: null, recordings: [
  { file: replay.file, bytes: 48_300_000, provider: "cboe", symbols: ["SPX", "SPY"], started: replay.started, delay_seconds: 900 },
  { file: "broken.oprec", bytes: 10, error: "recording truncated" },
] }
beforeEach(() => vi.mocked(useLive).mockReturnValue(liveState(status, null, "open")))
afterEach(() => { clients.splice(0).forEach((client) => client.clear()); vi.clearAllMocks(); vi.unstubAllGlobals(); dataSource.set("live"); activeAccount.set(MAIN_ACCOUNT) })

describe("trading a replay", () => {
  it("sends every route to the replay, but not its controls or an account", async () => {
    const fetcher = vi.fn(async () => new Response(JSON.stringify(portfolio), { status: 200 }))
    vi.stubGlobal("fetch", fetcher)
    activeAccount.set("swing-50k")
    dataSource.set("replay")
    await api.portfolio()
    expect(fetcher).toHaveBeenLastCalledWith("/api/replay/portfolio", expect.anything())
    await api.summary("SPX")
    expect(fetcher).toHaveBeenLastCalledWith("/api/replay/underlyings/SPX/summary", expect.anything())
    await api.status()
    expect(fetcher).toHaveBeenLastCalledWith("/api/replay/status", expect.anything())
    await api.replay()
    expect(fetcher).toHaveBeenLastCalledWith("/api/replay", expect.anything())
    await api.controlReplay({ paused: true }, "open")
    expect(fetcher).toHaveBeenLastCalledWith("/api/replay", expect.objectContaining({ method: "PUT", body: JSON.stringify({ paused: true }) }))
    dataSource.set("live")
    await api.portfolio()
    expect(fetcher).toHaveBeenLastCalledWith("/api/portfolio?account=swing-50k", expect.anything())
  })

  it("shows the replay's tick and state instead of the live one", () => {
    const replayTick = { type: "replay_tick", trading: status.trading, feed: { state: "delayed", message: "replay" },
      underlyings: [], engine: status.engine, replay } as unknown as Tick
    const state = liveState(status, replayTick, "open", 0, MAIN_ACCOUNT, () => {}, "replay", replay)
    expect(state.source).toBe("replay")
    expect(state.replay).toBe(replay)
    expect(state.tick).toBe(replayTick)
    expect(replayClock(replay.time)).toBe("Tue, Sep 22, 15:04:31 ET")
    expect(replayClock(null)).toBe("Not started")
  })
})

describe("the replay page", () => {
  it("lists recordings to replay, with a starting speed", () => {
    const html = render(<ReplayView />, recordings)
    expect(html).toContain("Trade a recorded day with its own paper account")
    expect(html).toContain(replay.file)
    expect(html).toContain("SPX, SPY")
    expect(html).toContain("48.3 MB")
    expect(html).toContain("Tue, Sep 22, 09:30:00 ET")
    expect(html).toContain("recording truncated")
    expect(html).toContain(`aria-label="Replay ${replay.file}"`)
    expect(html).toContain('aria-label="Starting speed"')
    expect(html).not.toContain("Now replaying")
  })

  it("explains how to record when there is nothing to replay", () => {
    const html = render(<ReplayView />, { ...recordings, recordings: [] })
    expect(html).toContain("--record-dir /home/trader/.openport/recordings")
  })

  it("controls a running replay and offers to trade it", () => {
    vi.mocked(useLive).mockReturnValue(liveState(status, null, "open", 0, MAIN_ACCOUNT, () => {}, "live", replay))
    const html = render(<ReplayView />, { ...recordings, replay })
    expect(html).toContain("Now replaying")
    expect(html).toContain("Tue, Sep 22, 15:04:31 ET")
    expect(html).toContain('aria-label="Replay speed"')
    expect(html).toContain(">Pause</button>")
    expect(html).toContain(">Skip gap</button>")
    expect(html).toContain("Trade this replay")
    expect(html).toContain(`aria-label="Replay ${replay.file}">Restart</button>`)
  })
})

describe("the replay banner", () => {
  it("shows while trading a replay, with the way back", () => {
    expect(render(<ReplayBanner />)).toBe("")
    vi.mocked(useLive).mockReturnValue(liveState(status, null, "open", 0, MAIN_ACCOUNT, () => {}, "replay", { ...replay, paused: true }))
    const html = render(<ReplayBanner />)
    expect(html).toContain("Tue, Sep 22, 15:04:31 ET")
    expect(html).toContain("paused")
    expect(html).toContain(">Resume</button>")
    expect(html).not.toContain("Skip gap")
    expect(html).toContain("Back to live")
  })
})
