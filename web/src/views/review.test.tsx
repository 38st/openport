// @vitest-environment jsdom
import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { act, type ReactNode } from "react"
import { createRoot, type Root } from "react-dom/client"
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest"
import { api } from "../api/client"
import { liveState, useLive } from "../api/live"
import type { Trade, TradeReview } from "../api/trading-types"
import { activeAccount } from "../lib/active-account"
import { dataSource } from "../lib/data-source"
import { strategyResults } from "../lib/journal"
import { tradeGroups } from "../lib/positions"
import { fill, order, status, trades, trading } from "../test/trading-fixtures"
import { Calendar, CsvDownloads, DayNoteEditor, StrategyRow, TradeDetail } from "./JournalView"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
vi.mock("../charts/useSize", () => ({ useSize: () => [{ current: null }, { width: 700, height: 200 }] }))
const review: TradeReview = { mae: "50.00", mfe: "300.00", planned_risk: "200.00", give_back: "31.50", heat: .25, r_multiple: 1.3425,
  worst: { pnl: "-50.00", time: "2026-09-22T14:20:00Z", spot: 6995 }, best: { pnl: "300.00", time: "2026-09-22T14:22:00Z", spot: 7010 } }
const trade: Trade = { ...trades[2]!, review, note: "Wait for confirmation", tags: ["patient"],
  entry_context: { spot: 7000, spot_source: "parity", iv: .2, delta: .5, years: .05, equity: "100000", floor_room: "2000", buying_power: "95000" },
  exit_context: { spot: 7008, spot_source: "quote", iv: .19, delta: .55, years: .049, equity: "100268.50", floor_room: null, buying_power: "99000" } }
let host: HTMLDivElement, root: Root, client: QueryClient
const render = async (node: ReactNode) => act(async () => root.render(<QueryClientProvider client={client}>{node}</QueryClientProvider>))
beforeEach(() => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  vi.mocked(useLive).mockReturnValue(liveState(status, null, "open"))
  host = document.createElement("div"); document.body.append(host); root = createRoot(host)
  client = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity } } })
  vi.spyOn(api, "candles").mockResolvedValue({ symbol: "SPX", interval: "1m", bars: [19, 20, 21, 22, 23].map((m) => ({
    t: Date.parse(`2026-09-22T14:${m}:00Z`) / 1000, o: 7000, h: 7010, l: 6990, c: 7000 + m - 19 })) })
})
afterEach(async () => {
  await act(async () => root.unmount()); host.remove(); client.clear()
  activeAccount.set("main"); dataSource.set("live"); vi.restoreAllMocks(); vi.unstubAllGlobals()
})

describe("trade review", () => {
  it("renders entry and exit context, extrema, notes and exact spot markers", async () => {
    client.setQueryData(["trade-candles", 0, "SPX", trading.account_version], await api.candles("SPX", "1m", 5000))
    await render(<TradeDetail trade={trade} fills={[fill]} trading={trading} />)
    expect(host.textContent).toContain("Entry context")
    expect(host.textContent).toContain("7000.00 · parity")
    expect(host.textContent).toContain("7008.00 · quote")
    expect(host.textContent).toContain("20.00% / 0.500")
    expect(host.textContent).toContain("spot 6995.00")
    expect(host.textContent).toContain("spot 7010.00")
    expect(host.textContent).toContain("25.0% / 1.34R")
    expect(host.textContent).toContain("$31.50")
    expect(host.querySelector("textarea")?.value).toBe("Wait for confirmation")
    expect(host.querySelector<HTMLInputElement>('input[placeholder="breakout, 0dte, fomc"]')?.value).toBe("patient")
    const chart = host.querySelector('[aria-label="Underlying across trade"]')
    for (const label of ["Entry", "Exit", "MAE", "MFE"]) expect(chart?.textContent).toContain(label)
    expect(chart?.querySelectorAll("circle").length).toBeGreaterThanOrEqual(4)
  })
  it("shows missing review as unavailable and labels simulated prices", async () => {
    vi.mocked(useLive).mockReturnValue(liveState({ ...status, provider: { ...status.provider, simulated: true } }, null, "open"))
    await render(<TradeDetail trade={trades[2]!} fills={[]} trading={trading} />)
    expect(host.textContent).toContain("Planned risk unavailable")
    expect(host.textContent).toContain("Not recorded")
    expect(host.textContent).toContain("Simulated prices")
    expect(host.textContent).not.toContain("0.00R")
  })
  it("computes strategy close and return from signed leg premiums", async () => {
    const long = { ...trade, id: "1", symbol: "long", opened_contracts: 2, closed_contracts: 2, cost: "800", average_close: "5", net: "198", fills: ["a"] }
    const short = { ...trade, id: "2", symbol: "short", direction: "short" as const, opened_contracts: 2, closed_contracts: 2, cost: "400", average_close: "2.50", net: "-102", fills: ["b"] }
    const combo = { ...order, id: "combo", filled_quantity: 2, average_fill_price: "2", legs: [{ symbol: "long", side: "buy" as const, ratio: 1 }, { symbol: "short", side: "sell" as const, ratio: 1 }] }
    const group = tradeGroups([long, short], [{ ...fill, id: "a", order_id: "combo" }, { ...fill, id: "b", order_id: "combo" }], [combo])[0]!
    expect(strategyResults(group.trades)).toEqual({ close: 2.5, return: .24 })
    await render(<table><tbody><StrategyRow group={group} expanded={false} onToggle={() => {}} /></tbody></table>)
    const cells = [...host.querySelectorAll("td")]
    expect(cells[7]?.textContent).toBe("$2.50")
    expect(cells[9]?.textContent).toContain("24.0%")
    expect(strategyResults([{ ...short, cost: "800" }, { ...long, cost: "400" }]).return).toBe(.24)
    expect(strategyResults([{ ...long, status: "open" }])).toEqual({ close: null, return: null })
    expect(strategyResults([{ ...long, cost: "0" }]).return).toBeNull()
    expect(strategyResults([{ ...long, opened_contracts: 4, closed_contracts: 4 },
      { ...short, opened_contracts: 8, closed_contracts: 8, average_close: "2" }], 2).close).toBe(2)
  })
  it("counts realised P&L and fees on partially closed strategy legs", () => {
    const open = { ...trade, status: "open" as const, closed: null, net: "40", unrealised: "30" }
    const group = tradeGroups([open], [fill], [order])[0]!
    expect(group.net).toBe(40); expect(group.unrealised).toBe(30)
  })
  it("downloads scoped CSV and routes demo downloads to the replay", async () => {
    activeAccount.set("practice")
    await render(<CsvDownloads scope="current" />)
    const links = [...host.querySelectorAll("a")]
    expect(links.map((a) => a.textContent)).toEqual(["Download trades CSV", "Download fills CSV"])
    expect(links[0]?.getAttribute("href")).toBe("/api/trades.csv?attempt=current&account=practice")
    expect(links[1]?.getAttribute("href")).toBe("/api/fills.csv?account=practice")
    expect(api.journalCsvUrl("trades", "2026-09-01", "2026-09-22", "all")).toContain("from=2026-09-01&to=2026-09-22&attempt=all&account=practice")
    dataSource.set("replay")
    expect(api.journalCsvUrl("fills")).toBe("/api/replay/fills.csv")
  })
  it("marks notes on days without trades and opens the day's plan and review", async () => {
    await render(<Calendar trades={[]} notes={{ "2026-09-22": { plan: "Wait for a pullback", review: "Kept risk small" } }} trading={trading} />)
    const day = host.querySelector<HTMLButtonElement>('[aria-label="2026-09-22, has day note"]')!
    expect(day.querySelector('[aria-label="Day note"]')).not.toBeNull()
    await act(async () => day.click())
    const fields = [...host.querySelectorAll("textarea")]
    expect(fields.map((f) => f.value)).toEqual(["Wait for a pullback", "Kept risk small"])
  })
  it("saves both day-note fields and shows server validation errors", async () => {
    const save = vi.spyOn(api, "annotateDay").mockRejectedValue(new Error("A note is at most 2,000 bytes of text"))
    await render(<DayNoteEditor day="2026-09-22" note={{ plan: "Wait", review: "Review" }} trading={trading} />)
    await act(async () => host.querySelector("form")!.dispatchEvent(new Event("submit", { bubbles: true, cancelable: true })))
    expect(save).toHaveBeenCalledWith("2026-09-22", { plan: "Wait", review: "Review" }, trading.write)
    expect(host.textContent).toContain("2,000 bytes")
  })
})
