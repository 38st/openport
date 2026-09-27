// @vitest-environment jsdom
import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { act, useState } from "react"
import { createRoot, type Root } from "react-dom/client"
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest"
import { api } from "../api/client"
import { liveState, useLive } from "../api/live"
import type { NewOrder, OrderPreview } from "../api/trading-types"
import { status, trading } from "../test/trading-fixtures"
import { OrderPreviewPanel, useOrderPreview } from "./OrderPreview"
vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
const preview: OrderPreview = { account_version: "17", decision: "ok", reason: null, buying_power: { required: "501", before: "10000", after: "9499" },
  exposure_change: { dollar_delta: 100, dollar_gamma_1pct: 1, vega: 10, theta: -5 }, max_loss: "501", max_loss_basis: "expiry_payoff", equity_at_max_loss: "9499", breaches_floor: true,
  breaches_soft_floor: false, max_units: 3, simulated: true, breach: { room: "1000", soft_room: null, complete: true, model: "reflection estimate", underlyings: [] } }
const order: NewOrder = { client_order_id: "preview:test", symbol: "SPXW  261022C05000000", side: "buy", type: "market", quantity: 1, time_in_force: "ioc" }
let root: Root, host: HTMLDivElement, client: QueryClient
beforeEach(() => {
  vi.useFakeTimers()
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  vi.mocked(useLive).mockReturnValue(liveState(status, null, "open"))
  client = new QueryClient({ defaultOptions: { queries: { retry: false } } })
  host = document.createElement("div"); document.body.append(host); root = createRoot(host)
})
afterEach(async () => { await act(async () => root.unmount()); host.remove(); client.clear(); vi.useRealTimers(); vi.restoreAllMocks() })
async function tick() { await act(async () => { await vi.advanceTimersByTimeAsync(350) }); await act(async () => { await vi.advanceTimersByTimeAsync(10) }) }
function Harness() {
  const [quantity, setQuantity] = useState(1)
  const result = useOrderPreview({ ...order, quantity }, trading)
  return <><output>{quantity}</output><OrderPreviewPanel preview={result} onSize={setQuantity} /></>
}
describe("order preview", () => {
  it("debounces, renders server numbers and sets quantity with Size to floor", async () => {
    const request = vi.spyOn(api, "previewOrder").mockResolvedValue(preview)
    await act(async () => root.render(<QueryClientProvider client={client}><Harness /></QueryClientProvider>))
    expect(request).not.toHaveBeenCalled()
    await tick()
    expect(request).toHaveBeenCalledOnce()
    expect(host.textContent).toContain("$9,499.00")
    expect(host.textContent).toContain("could breach the plan floor")
    await act(async () => host.querySelector("button")!.click())
    expect(host.querySelector("output")!.textContent).toBe("3")
    expect(host.textContent).not.toContain("$9,499.00")
    await tick()
    expect(request).toHaveBeenLastCalledWith(expect.objectContaining({ quantity: 3 }), trading.write)
  })
  it("shows failure without guessing and disables size", async () => {
    vi.spyOn(api, "previewOrder").mockRejectedValue(new Error("offline"))
    await act(async () => root.render(<QueryClientProvider client={client}><Harness /></QueryClientProvider>))
    await tick()
    expect(host.textContent).toContain("Preview failed")
    expect(host.textContent).not.toContain("$9,499.00")
    expect(host.querySelector("button")!.disabled).toBe(true)
  })
  it("labels scenario estimates and prevents zero-unit sizing", async () => {
    const result = { ...preview, max_loss_basis: "scenario_grid" as const, max_units: 0 }
    await act(async () => root.render(<OrderPreviewPanel preview={{ data: result, error: null, loading: false }} onSize={() => {}} />))
    expect(host.textContent).toContain("Scenario-grid loss")
    expect(host.querySelector("button")!.disabled).toBe(true)
  })
})
