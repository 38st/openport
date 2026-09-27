// @vitest-environment jsdom
import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { act, type ReactNode } from "react"
import { createRoot, type Root } from "react-dom/client"
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest"
import { api } from "../api/client"
import { liveState, useLive } from "../api/live"
import type { PlaybooksResponse, PlaybookStats } from "../api/playbook-types"
import { PassOddsCard, StagedOrders } from "../components/Playbooks"
import { trades, status } from "../test/trading-fixtures"
import { JournalView } from "./JournalView"
import { newPlaybook, PlaybooksView } from "./PlaybooksView"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
vi.mock("../charts/useSize", () => ({ useSize: () => [{ current: null }, { width: 700, height: 200 }] }))
const stats: PlaybookStats = { trades: 2, win_rate: .5, average_win: "100", average_loss: "-50", expectancy: "25", profit_factor: 2, no_losses: false, average_r: .25, adherence: .8 }
function catalogue(): PlaybooksResponse {
  return { definitions: { "put-spread": { versions: [{ ...newPlaybook, version: 1 }], deleted: false } }, modes: { "put-spread": "stage" }, auto_allowed: false,
    reasons: { "put-spread:SPX": "Ready" }, reports: { "put-spread": { all: stats, followed: stats, deviated: { ...stats, trades: 0 }, trades: [] } },
    staged: [{ id: "stage-1", playbook: "put-spread", version: 1, name: "Morning put spread", underlying: "SPX", units: 2, net: "-1.20", max_loss: "760", max_loss_basis: "expiry_payoff", close_by: "2026-09-22T19:45:00Z", simulated: true,
      legs: [{ symbol: "synthetic-put", side: "sell", ratio: 1, strike: 5900, type: "put", expiry: "2026-09-22PM" }] }] }
}
let root: Root, host: HTMLDivElement, client: QueryClient
beforeEach(() => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  vi.mocked(useLive).mockReturnValue(liveState(status, null, "open"))
  host = document.createElement("div"); document.body.append(host); root = createRoot(host)
  client = new QueryClient({ defaultOptions: { queries: { retry: false, gcTime: Infinity, staleTime: Infinity } } })
  vi.spyOn(api, "playbooks").mockResolvedValue(catalogue())
  vi.spyOn(api, "passOdds").mockRejectedValue(new Error("Pass odds need at least 10 completed days with recorded intraday lows and highs"))
  vi.spyOn(api, "savePlaybook").mockResolvedValue(catalogue())
  vi.spyOn(api, "playbookMode").mockResolvedValue(catalogue())
  vi.spyOn(api, "stagedAction").mockImplementation(async () => { const value = { ...catalogue(), staged: [] }; vi.mocked(api.playbooks).mockResolvedValue(value); return value })
})
afterEach(async () => { await act(async () => root.unmount()); host.remove(); client.clear(); vi.restoreAllMocks(); vi.unstubAllGlobals() })
async function flush() { await act(async () => { await new Promise((resolve) => setTimeout(resolve, 20)) }) }
async function render(node: ReactNode) { await act(async () => root.render(<QueryClientProvider client={client}>{node}</QueryClientProvider>)); await flush() }
function button(text: string) { const found = [...host.querySelectorAll("button")].find((item) => item.textContent === text); if (!found) throw new Error(`Missing ${text}`); return found }
async function click(text: string) { await act(async () => button(text).click()); await flush() }
async function value(selector: string, text: string) {
  const field = host.querySelector(selector) as HTMLTextAreaElement | HTMLSelectElement
  const prototype = field instanceof HTMLTextAreaElement ? HTMLTextAreaElement.prototype : HTMLSelectElement.prototype
  await act(async () => { Object.getOwnPropertyDescriptor(prototype, "value")!.set!.call(field, text); field.dispatchEvent(new Event(field instanceof HTMLTextAreaElement ? "input" : "change", { bubbles: true })) })
}
describe("Playbooks page and staged actions", () => {
  it("shows saved versions, safe live modes and followed-versus-deviated statistics", async () => {
    await render(<PlaybooksView />)
    expect(host.textContent).toContain("Morning put spread · v1")
    expect(host.querySelector<HTMLOptionElement>('option[value="auto"]')?.disabled).toBe(true)
    await click("Stats and versions")
    for (const text of ["Followed rules", "Deviated", "Expectancy", "80.0%", "Saved versions", "Estimate from past results, not a prediction", "at least 10 completed days"]) expect(host.textContent).toContain(text)
    expect(api.passOdds).toHaveBeenCalledWith(20, 1000, "put-spread", expect.any(AbortSignal))
  })
  it("keeps archived statistics and versions readable without offering new entries", async () => {
    const saved = catalogue()
    saved.definitions["put-spread"]!.deleted = true
    saved.staged = []
    vi.mocked(api.playbooks).mockResolvedValue(saved)
    await render(<PlaybooksView />)
    expect(host.querySelector('select[aria-label="Mode for Morning put spread"]')).toBeNull()
    await click("Morning put spread · archived stats and versions")
    expect(host.textContent).toContain("Saved versions")
    expect(host.textContent).toContain("Followed rules")
    expect(host.textContent).toContain("Version 1")
  })
  it("saves an edited version and displays JSON errors without losing the editor", async () => {
    await render(<PlaybooksView />)
    await click("Edit Morning put spread")
    await value('textarea[aria-label="Playbook definition"]', "{")
    await click("Save version")
    expect(host.querySelector('[role="alert"]')).not.toBeNull()
    expect(api.savePlaybook).not.toHaveBeenCalled()
    const edit = { ...newPlaybook, version: 1, name: "Changed setup" }
    await value('textarea[aria-label="Playbook definition"]', JSON.stringify(edit))
    await click("Save version")
    expect(api.savePlaybook).toHaveBeenCalledWith(edit, "open")
    expect(host.querySelector("textarea")).toBeNull()
  })
  it("stages on the selected account without sending when its mode changes", async () => {
    await render(<PlaybooksView />)
    await value('select[aria-label="Mode for Morning put spread"]', "stage")
    expect(api.playbookMode).toHaveBeenCalledWith("put-spread", "stage", "open")
    expect(api.stagedAction).not.toHaveBeenCalled()
  })
  it.each(["send", "dismiss"] as const)("%s uses the server stage token and removes the stage after refresh", async (action) => {
    await render(<StagedOrders />)
    expect(host.textContent).toContain("Simulated orders")
    expect(host.textContent).toContain("playbook:put-spread@v1")
    await click(`${action === "send" ? "Send" : "Dismiss"} Morning put spread`)
    expect(api.stagedAction).toHaveBeenCalledWith("stage-1", action, "open")
    expect(host.textContent).not.toContain("Staged playbook orders")
  })
  it("explains stale-stage refusal and keeps the order unsent", async () => {
    vi.mocked(api.stagedAction).mockRejectedValue(new Error("Staged order expired or changed; refresh playbooks"))
    await render(<StagedOrders />)
    await click("Send Morning put spread")
    expect(host.querySelector('[role="alert"]')?.textContent).toContain("expired or changed")
  })
  it("shows pass, fail, neither, history count and seed as estimates", async () => {
    vi.mocked(api.passOdds).mockResolvedValue({ pass: .25, fail: .1, neither: .65, median_days_to_pass: 9, historical_days: 15, seed: "81723", days: 20, samples: 1000, label: "Estimate from past results, not a prediction", history_basis: "Account equity days", path_assumption: "Observed extrema only", simulated: true })
    await render(<PassOddsCard />)
    for (const text of ["25.0%", "10.0%", "65.0%", "Median days to pass: 9", "15 historical days", "seed 81723", "not a prediction"]) expect(host.textContent).toContain(text)
  })
  it("filters Journal across all versions of a playbook while retaining other filters", async () => {
    vi.spyOn(api, "trades").mockResolvedValue({ account_version: "17", attempt: 1, trades: [
      { ...trades[2]!, id: "pb1", tags: ["playbook:put-spread@v1"] },
      { ...trades[2]!, id: "pb2", tags: ["playbook:put-spread@v2"] },
      { ...trades[2]!, id: "other", tags: ["playbook:other@v1"] },
    ], share_trades: [], day_notes: {} })
    vi.spyOn(api, "orders").mockResolvedValue({ account_version: "17", orders: [] })
    vi.spyOn(api, "fills").mockResolvedValue({ account_version: "17", fills: [] })
    await render(<JournalView />)
    expect(host.textContent).toContain("3 closed trades")
    await value('select[aria-label="Playbook"]', "put-spread")
    expect(host.textContent).toContain("2 closed trades")
    await value('select[aria-label="Tag"]', "playbook:put-spread@v1")
    expect(host.textContent).toContain("1 closed trade")
  })
})
