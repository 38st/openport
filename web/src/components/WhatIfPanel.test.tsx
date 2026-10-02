// @vitest-environment jsdom
import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { act } from "react"
import { createRoot, type Root } from "react-dom/client"
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest"
import { api } from "../api/client"
import { liveState, useLive } from "../api/live"
import type { NewOrder, WhatIfAccount, WhatIfResponse } from "../api/trading-types"
import { addWhatIfOrder, updateWhatIf } from "../lib/what-if"
import { portfolio, status, trading } from "../test/trading-fixtures"
import { WhatIfPanel } from "./WhatIfPanel"
vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))

const scope = "live:main"
const position = portfolio.positions[0]!
const account = (equity: string, delta: number, loss: string, pnl: number[]): WhatIfAccount => ({
  equity, buying_power: "9000.00", exposure: { dollar_delta: delta, dollar_gamma_1pct: 10, vega: 5, theta: -2 },
  max_loss: loss, equity_at_max_loss: String(10000 - Number(loss)), breaches_floor: false, breaches_soft_floor: null,
  scenarios: { spot_percent: [-5, 0, 5], vol_points: [-5, 0, 5], pnl: pnl.map((value) => [value - 1, value, value + 1]), complete: true },
  breach: { room: String(Number(equity) - 9000), soft_room: null, complete: true, model: "reflection estimate", underlyings: [] },
})
const response: WhatIfResponse = {
  account_version: "17", simulated: true, current: account("10000.00", 5000, "800.00", [-800, 0, 600]),
  candidates: [
    { name: "Close all", decision: "ok", reason: null, orders: [{ decision: "ok", reason: null }], after: account("9978.70", 0, "21.30", [-21.3, -21.3, -21.3]) },
    { name: "Buy 22", decision: "BUYING_POWER", reason: { code: "BUYING_POWER", message: "Insufficient buying power" },
      orders: [{ decision: "BUYING_POWER", reason: { code: "BUYING_POWER", message: "Insufficient buying power" } }], after: null },
  ],
}
const more: NewOrder = { client_order_id: "preview:single", symbol: position.symbol, side: "buy", type: "market", time_in_force: "ioc", quantity: 22 }
let root: Root, host: HTMLDivElement, client: QueryClient
beforeEach(() => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  vi.mocked(useLive).mockReturnValue(liveState(status, null, "open"))
  updateWhatIf(scope, () => ({ candidates: [], target: null }))
  client = new QueryClient({ defaultOptions: { queries: { retry: false } } })
  host = document.createElement("div"); document.body.append(host); root = createRoot(host)
})
afterEach(async () => { await act(async () => root.unmount()); host.remove(); client.clear(); vi.restoreAllMocks() })
const render = () => act(async () => root.render(<QueryClientProvider client={client}><WhatIfPanel positions={[position]} trading={trading} /></QueryClientProvider>))
const button = (text: string) => [...host.querySelectorAll("button")].find((b) => b.textContent?.includes(text))!

describe("what-if", () => {
  it("builds candidates from positions and tickets, and compares them against the book", async () => {
    await render()
    expect(host.textContent).toContain("Build each candidate from a ticket's preview")
    await act(async () => button("Add closing every position").click())
    // A ticket's order joins as a new candidate, its client ID dropped.
    await act(async () => { addWhatIfOrder(scope, more, "Buy 22") })
    expect(host.textContent).toContain(`Sell ${position.quantity} SPX`)
    expect(host.textContent).toContain("Buy 22 SPX")
    const request = vi.spyOn(api, "whatIf").mockResolvedValue(response)
    await act(async () => button("Compare 2 candidates").click())
    expect(request).toHaveBeenCalledOnce()
    const sent = request.mock.calls[0]![0]
    expect(sent.map((c) => c.name)).toEqual(["Close all", "Buy 22"])
    expect(sent[1]!.orders[0]).toMatchObject({ client_order_id: "", quantity: 22 })
    const table = host.querySelector("table")!.textContent!
    expect(table).toContain("Now")
    expect(table).toContain("$9,978.70")
    expect(table).toContain("$21.30")
    // Each candidate's change in dollar delta from now.
    expect(table).toContain("-5000")
    expect(table).toContain("BUYING_POWER: Insufficient buying power")
    expect(table).toContain("Cannot be projected")
    expect(host.textContent).toContain("P&L from today's equity by spot move, volatility unchanged")
    expect(host.textContent).toContain("+5%")
  })
  it("adds a ticket's orders to a picked candidate, up to four", async () => {
    await act(async () => { addWhatIfOrder(scope, more, "First") })
    await render()
    await act(async () => { host.querySelector<HTMLInputElement>('input[type="checkbox"]')!.click() })
    for (let i = 0; i < 3; ++i) await act(async () => { addWhatIfOrder(scope, { ...more, quantity: i + 1 }, "ignored") })
    expect(host.querySelectorAll("ol > li")).toHaveLength(1)
    expect(host.querySelectorAll("ol > li li")).toHaveLength(4)
    let added = true
    await act(async () => { added = addWhatIfOrder(scope, more, "ignored") })
    expect(added).toBe(false)
    await act(async () => button("Remove").click())
    expect(host.querySelectorAll("ol > li")).toHaveLength(0)
  })
})
