// @vitest-environment jsdom
import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { act } from "react"
import { createRoot, type Root } from "react-dom/client"
import { afterEach, beforeEach, expect, it, vi } from "vitest"
import { api } from "../api/client"
import { liveState, useLive } from "../api/live"
import * as connection from "../api/connection"
import type { ReplayListing, ReplayState, Tick } from "../api/types"
import { ReplayBanner } from "../components/ReplayBanner"
import { status } from "../test/trading-fixtures"
import { writeToken } from "../lib/write-token"
import { dataSource } from "../lib/data-source"
import { ReplayView } from "./ReplayView"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
let root: Root
let host: HTMLDivElement
let client: QueryClient
const navigate = vi.fn()
const switchSource = vi.fn()
const replay: ReplayState = { id: "run-1", file: "Demo market: Selloff", demo: true, scenario: "selloff", seed: "18446744073709551615", generator: 1,
  date: "2026-09-16", start_at: "15:00", provider: "demo", plan: "intraday-25k", symbols: ["SPX"], started: "2026-09-16T13:30:00Z",
  delay_seconds: 0, speed: 60, paused: true, finished: false, time: "2026-09-16T19:00:00Z" }
const listing: ReplayListing = { write: "open", directory: "", recordings: [], replay: null,
  demos: [{ id: "selloff", title: "Selloff", description: "A simulated decline.", goal: "Keep risk contained.", symbols: ["SPX"], provider: "demo", started: "2026-09-16T13:30:00Z" }],
  history: [{ ...replay, id: "saved-run", plan_name: "Intraday 25K", finished: true, result: "fail", pnl: "-123.450000", read_only: true }] }
beforeEach(() => {
  writeToken.set("")
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  Object.defineProperty(HTMLDialogElement.prototype, "showModal", { configurable: true, value() { this.open = true } })
  Object.defineProperty(HTMLDialogElement.prototype, "close", { configurable: true, value() { this.open = false } })
  host = document.createElement("div"); document.body.append(host); root = createRoot(host)
  client = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity } } })
  client.setQueryData(["replay-listing"], listing)
  client.setQueryData(["plans"], { plans: [{ id: "intraday-25k", name: "Intraday 25K", rules: { phase: "evaluation" } }] })
  vi.mocked(useLive).mockReturnValue(liveState(status, null, "open", 0, "main", () => {}, "live", null, switchSource))
  vi.spyOn(api, "replay").mockResolvedValue(listing)
  vi.spyOn(api, "startReplay").mockResolvedValue({ replay })
  vi.spyOn(api, "deleteReplay").mockResolvedValue({ deleted: "saved-run" })
})
afterEach(async () => {
  await act(async () => root.unmount()); host.remove(); client.clear()
  writeToken.set("")
  vi.restoreAllMocks(); vi.clearAllMocks(); vi.unstubAllGlobals(); dataSource.set("live")
})
async function render() { await act(async () => root.render(<QueryClientProvider client={client}><ReplayView onNavigate={navigate} /></QueryClientProvider>)) }
async function change(label: string, value: string) {
  const element = host.querySelector(`[aria-label="${label}"]`) as HTMLInputElement
  await act(async () => {
    const prototype = element.tagName === "SELECT" ? HTMLSelectElement.prototype : HTMLInputElement.prototype
    Object.getOwnPropertyDescriptor(prototype, "value")!.set!.call(element, value)
    element.dispatchEvent(new Event(element.tagName === "SELECT" ? "change" : "input", { bubbles: true }))
  })
}
async function click(text: string) {
  const button = [...host.querySelectorAll("button")].find((b) => b.textContent === text)!
  expect(button).toBeDefined()
  await act(async () => button.click())
}

it("submits plan, exact typed uint64 seed, drill time and paused state", async () => {
  await render()
  await change("Replay plan", "intraday-25k")
  await change("Start at", "15:00")
  await change("Seed mode", "typed")
  await change("Seed", "18446744073709551615")
  await act(async () => (host.querySelector('input[type="checkbox"]') as HTMLInputElement).click())
  await click("Start")
  expect(api.startReplay).toHaveBeenCalledWith({ demo: "selloff" }, 10, "open", {
    plan: "intraday-25k", start_at: "15:00", seed: "18446744073709551615", paused: true,
  })
  expect(switchSource).toHaveBeenCalledWith("replay")
})

it("omits fresh seeds, requests the scenario seed, and rejects out of range typed seeds", async () => {
  await render()
  await click("Start")
  expect(api.startReplay).toHaveBeenLastCalledWith({ demo: "selloff" }, 10, "open", { plan: "practice", paused: false })
  await change("Seed mode", "scenario"); await click("Start")
  expect(api.startReplay).toHaveBeenLastCalledWith({ demo: "selloff" }, 10, "open", { plan: "practice", paused: false, seed: "scenario" })
  await change("Seed mode", "typed"); await change("Seed", "18446744073709551616")
  expect((host.querySelector('[aria-label="Start Selloff"]') as HTMLButtonElement).disabled).toBe(true)
})

it("shows history result and P&L, opens both views read-only, and confirms deletion", async () => {
  await render()
  expect(host.textContent).toContain("fail")
  expect(host.textContent).toContain("123.45")
  expect(host.textContent).toContain("simulated · seed 18446744073709551615")
  expect(host.textContent).toContain("2026-09-16 · 15:00 · Intraday 25K")
  await click("Open journal")
  expect(switchSource).toHaveBeenCalledWith("history:saved-run")
  expect(navigate).toHaveBeenCalledWith("journal")
  await click("Open dashboard")
  expect(navigate).toHaveBeenCalledWith("dashboard")
  vi.mocked(useLive).mockReturnValue(liveState(status, null, "open", 0, "main", () => {}, "history:saved-run", { ...replay, id: "saved-run" }, switchSource))
  await render()
  await click("Delete")
  expect(api.deleteReplay).not.toHaveBeenCalled()
  await click("Cancel")
  expect(api.deleteReplay).not.toHaveBeenCalled()
  await click("Delete"); await click("Delete run")
  expect(api.deleteReplay).toHaveBeenCalledWith("saved-run", "open")
  expect(switchSource).toHaveBeenLastCalledWith("live")
})

it("shows fast-forward progress and repeats the displayed seed", async () => {
  vi.mocked(useLive).mockReturnValue(liveState(status, null, "open", 0, "main", () => {}, "replay", { ...replay, fast_forwarding: true, progress: .5 }, switchSource))
  await act(async () => root.render(<QueryClientProvider client={client}><ReplayBanner /></QueryClientProvider>))
  expect(host.textContent).toContain("Preparing start state… 50%")
  expect(host.textContent).toContain("seed 18446744073709551615")
  expect(host.textContent).not.toContain("Skip gap")
  await click("Replay this seed")
  expect(api.startReplay).toHaveBeenCalledWith({ demo: "selloff" }, 60, "open", {
    seed: replay.seed, plan: replay.plan, start_at: "15:00", date: "2026-09-16",
  })
})

it("shows a lockstep step in progress and disables trading until it settles", async () => {
  const stepping = liveState(status, null, "open", 0, "main", () => {}, "replay", { ...replay, stepping: true }, switchSource)
  expect(stepping.trading?.write).toBe("disabled")
  expect(stepping.trading?.reason).toBe("REPLAY_STEPPING")
  vi.mocked(useLive).mockReturnValue(stepping)
  await act(async () => root.render(<QueryClientProvider client={client}><ReplayBanner /></QueryClientProvider>))
  expect(host.textContent).toContain("Stepping…")
  expect(host.textContent).not.toContain("Resume")
  expect(host.textContent).not.toContain("Preparing start state")
})

it("routes archived account reads separately from both the live account and the running replay", async () => {
  const fetcher = vi.fn(async () => new Response("{}", { status: 200 }))
  vi.stubGlobal("fetch", fetcher)
  dataSource.set("history:saved-run")
  await api.account()
  expect(fetcher).toHaveBeenLastCalledWith("/api/replay/history/saved-run/account", expect.anything())
  await api.trades()
  expect(fetcher).toHaveBeenLastCalledWith("/api/replay/history/saved-run/trades?status=all&attempt=current", expect.anything())
  await api.status()
  expect(fetcher).toHaveBeenLastCalledWith("/api/replay/history/saved-run/status", expect.anything())
})

it("changes account scope when another browser starts a new replay run", async () => {
  const actual = await vi.importActual<typeof import("../api/live")>("../api/live")
  let receive: (tick: Tick | null) => void = () => {}
  vi.spyOn(connection, "connectLive").mockImplementation((_url, _client, _tick, connected, replayTick) => {
    receive = replayTick!
    connected("open")
    return () => {}
  })
  vi.spyOn(api, "status").mockResolvedValue(status)
  function Scope() { return <output>{actual.useLive().accountScope}</output> }
  dataSource.set("replay")
  await act(async () => root.render(<QueryClientProvider client={client}><actual.LiveProvider><Scope /></actual.LiveProvider></QueryClientProvider>))
  const tick = { type: "replay_tick", trading: status.trading, underlyings: [], engine: status.engine, replay } as unknown as Tick
  await act(async () => receive(tick))
  const previous = Number(host.querySelector("output")!.textContent)
  client.setQueryData(["trading", previous, "fills", "1"], { fills: ["old-run"] })
  await act(async () => receive({ ...tick, replay: { ...replay, id: "run-2" } }))
  expect(Number(host.querySelector("output")!.textContent)).toBe(previous + 1)
  expect(client.getQueryData(["trading", previous, "fills", "1"])).toBeUndefined()
})

it("blocks replay starts and every running control until a required token is held", async () => {
  const required = { ...listing, write: "token" as const }
  client.setQueryData(["replay-listing"], required)
  vi.mocked(api.replay).mockResolvedValue(required)
  vi.spyOn(api, "controlReplay").mockResolvedValue({ replay })
  vi.spyOn(api, "stopReplay").mockResolvedValue({ replay: null })
  await render()
  const start = host.querySelector('[aria-label="Start Selloff"]') as HTMLButtonElement
  expect(start.disabled).toBe(true)
  await act(async () => start.click())
  expect(api.startReplay).not.toHaveBeenCalled()
  const running = { ...required, replay: { ...replay, paused: false } }
  vi.mocked(api.replay).mockResolvedValue(running)
  client.setQueryData(["replay-listing"], running)
  await render()
  for (const label of ["Pause", "Skip gap", "Stop"]) {
    const button = [...host.querySelectorAll("button")].find(element => element.textContent === label)!
    expect(button.disabled).toBe(true)
    await act(async () => button.click())
  }
  const speed = host.querySelector('[aria-label="Replay speed"] button') as HTMLButtonElement
  expect(speed.matches(":disabled")).toBe(true)
  await act(async () => { start.click(); speed.click() })
  expect(api.startReplay).not.toHaveBeenCalled()
  expect(api.controlReplay).not.toHaveBeenCalled()
  expect(api.stopReplay).not.toHaveBeenCalled()
  await act(async () => { writeToken.set("write-token") })
  expect(speed.matches(":disabled")).toBe(false)
  await click("Pause")
  expect(api.controlReplay).toHaveBeenCalledWith({ paused: true }, "token")
})

it("labels a run's sessions by date and steps to a New York date and time", async () => {
  const sessions = [
    { session: "regular" as const, date: "2026-09-16", open: "2026-09-16T13:30:00Z", end: "2026-09-16T20:15:00Z" },
    { session: "curb" as const, date: "2026-09-16", open: "2026-09-16T20:15:15Z", end: "2026-09-16T20:59:45Z" },
    { session: "overnight" as const, date: "2026-09-17", open: "2026-09-17T00:15:00Z", end: "2026-09-17T13:25:00Z" },
  ]
  const several = { ...listing, demos: [{ ...listing.demos![0]!, sessions, end: sessions[2]!.end }], replay: { ...replay, sessions, end: sessions[2]!.end } }
  client.setQueryData(["replay-listing"], several)
  vi.mocked(api.replay).mockResolvedValue(several)
  vi.spyOn(api, "controlReplay").mockResolvedValue({ replay })
  await render()
  expect(host.textContent).toContain("simulated · Wed, Sep 16 regular, curb · Thu, Sep 17 overnight")
  const marked = host.querySelector('[aria-label="Run sessions"] [aria-current="step"]')
  expect(marked?.textContent).toBe("Wed, Sep 16 regular")
  // The field starts at the replay's clock in New York.
  expect((host.querySelector('[aria-label="Step to"]') as HTMLInputElement).value).toBe("2026-09-16T15:00")
  await change("Step to", "2026-09-17T02:30")
  await click("Step")
  expect(api.controlReplay).toHaveBeenCalledWith({ until: "2026-09-17T02:30" }, "open")
  // Between sessions the banner names the next one.
  vi.mocked(useLive).mockReturnValue(liveState(status, null, "open", 0, "main", () => {}, "replay", { ...replay, sessions, time: "2026-09-16T22:00:00Z" }, switchSource))
  await act(async () => root.render(<QueryClientProvider client={client}><ReplayBanner /></QueryClientProvider>))
  expect(host.textContent).toContain("closed until the overnight session · 3 of 3")
})
