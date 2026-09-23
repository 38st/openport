// @vitest-environment jsdom
import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { act } from "react"
import { createRoot, type Root } from "react-dom/client"
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest"
import { api, ApiError } from "../api/client"
import { liveState, useLive } from "../api/live"
import type { TradingStatus } from "../api/trading-types"
import { order, quote, selection, status, trading } from "../test/trading-fixtures"
import { OrderTicket } from "./OrderTicket"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
vi.mock("../api/trading", async (original) => ({ ...await original<typeof import("../api/trading")>(), useRefreshTrading: () => vi.fn() }))

let root: Root
let host: HTMLDivElement
let client: QueryClient
const scroll = vi.fn()
beforeEach(() => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  vi.mocked(useLive).mockReturnValue(liveState(status, null, "open"))
  vi.spyOn(api, "orders").mockResolvedValue({ account_version: "17", orders: [] })
  vi.spyOn(api, "submitOrder").mockResolvedValue({ account_version: "18", order, fills: [] })
  Object.defineProperty(HTMLElement.prototype, "scrollIntoView", { configurable: true, value: scroll })
  Object.defineProperty(HTMLDialogElement.prototype, "showModal", { configurable: true, value() { this.open = true } })
  Object.defineProperty(HTMLDialogElement.prototype, "close", { configurable: true, value() { this.open = false } })
  host = document.createElement("div")
  document.body.append(host)
  root = createRoot(host)
  client = new QueryClient({ defaultOptions: { queries: { retry: false, gcTime: Infinity } } })
})
afterEach(async () => {
  await act(async () => root.unmount())
  client.clear()
  host.remove()
  scroll.mockClear()
  vi.restoreAllMocks()
  vi.unstubAllGlobals()
})
async function render(config: TradingStatus = trading) {
  await act(async () => root.render(<QueryClientProvider client={client}>
    <OrderTicket selection={selection} quote={quote} trading={config} onClose={() => {}} />
  </QueryClientProvider>))
}
function button(name: string) {
  const found = [...host.querySelectorAll("button")].find((b) => b.textContent === name || b.getAttribute("aria-label") === name)
  if (!found) throw new Error(`Missing button: ${name}`)
  return found
}
function field(name: string): HTMLInputElement | HTMLSelectElement {
  const found = [...host.querySelectorAll("label")].find((label) => label.textContent?.startsWith(name))?.querySelector("input, select")
  if (!(found instanceof HTMLInputElement || found instanceof HTMLSelectElement)) throw new Error(`Missing field: ${name}`)
  return found
}
async function setField(name: string, value: string) {
  const element = field(name)
  const prototype = element instanceof HTMLInputElement ? HTMLInputElement.prototype : HTMLSelectElement.prototype
  await act(async () => {
    Object.getOwnPropertyDescriptor(prototype, "value")!.set!.call(element, value)
    element.dispatchEvent(new Event(element instanceof HTMLInputElement ? "input" : "change", { bubbles: true }))
  })
}
async function click(name: string) { await act(async () => button(name).click()) }

describe("order ticket interaction", () => {
  it.each([400, 403, 404, 409, 422])("starts a fresh ID after HTTP %s and keeps editable form values", async (status) => {
    vi.mocked(api.submitOrder).mockRejectedValueOnce(new ApiError(status, "Delta cap exceeded", "DELTA_LIMIT", 200, 100, "aggregate"))
    await render()
    await setField("Side", "sell")
    await setField("Quantity", "3")
    await setField("Time in force", "ioc")
    await setField("Limit price", "15.80")
    await click("Submit paper order")
    const first = vi.mocked(api.submitOrder).mock.calls[0]![0]
    expect(first).toMatchObject({ side: "sell", quantity: 3, time_in_force: "ioc", limit_price: "15.80" })
    expect(host.textContent).toContain("DELTA_LIMIT: Delta cap exceeded")
    expect(host.textContent).toContain("Actual 200.00 · Limit 100.00 · aggregate")
    expect(host.textContent).not.toContain("Retry same order")
    const result = host.querySelector('[aria-label="Order result"]')!
    expect(document.activeElement).toBe(result)
    expect(scroll).toHaveBeenCalledWith({ block: "start" })
    expect(result.compareDocumentPosition(host.querySelector("form")!) & Node.DOCUMENT_POSITION_FOLLOWING).not.toBe(0)
    await click("New order")
    expect(host.querySelector("fieldset")?.disabled).toBe(false)
    expect(document.activeElement).toBe(field("Side"))
    expect(field("Side").value).toBe("sell")
    expect(field("Quantity").value).toBe("3")
    expect(field("Time in force").value).toBe("ioc")
    expect(field("Limit price").value).toBe("15.80")
    expect(host.textContent).not.toContain("DELTA_LIMIT")
    await click("Submit paper order")
    const second = vi.mocked(api.submitOrder).mock.calls[1]![0]
    expect(second.client_order_id).not.toBe(first.client_order_id)
    expect({ ...second, client_order_id: first.client_order_id }).toEqual(first)
  })
  it.each([
    new TypeError("Failed to fetch"), new DOMException("Timed out", "TimeoutError"),
    new DOMException("Aborted", "AbortError"), new ApiError(503, "Busy"),
    new ApiError(408, "Request timeout"), new ApiError(504, "Gateway timeout"),
  ])("retries an interrupted request with its frozen ID and values: %s", async (error) => {
    vi.mocked(api.submitOrder).mockRejectedValueOnce(error)
    await render()
    await click("Submit paper order")
    expect(host.querySelector("fieldset")?.disabled).toBe(true)
    expect(host.textContent).not.toContain("New order")
    await click("Retry same order")
    expect(api.submitOrder).toHaveBeenCalledTimes(2)
    expect(vi.mocked(api.submitOrder).mock.calls[1]![0]).toBe(vi.mocked(api.submitOrder).mock.calls[0]![0])
    expect(host.textContent).toContain("Partial fill")
  })
  it("does not offer an automatic retry or fresh ID for an unknown server failure", async () => {
    vi.mocked(api.submitOrder).mockRejectedValueOnce(new ApiError(500, "Unexpected response"))
    await render()
    await click("Submit paper order")
    expect(host.textContent).not.toContain("Retry same order")
    expect(host.textContent).not.toContain("New order")
    expect(host.textContent).toContain("Check Portfolio to confirm")
  })
  it("retains INVALID_TICK feedback for typed off-tick prices", async () => {
    vi.mocked(api.submitOrder).mockRejectedValueOnce(new ApiError(422, "Limit price is not a positive multiple of the product tier tick", "INVALID_TICK"))
    await render()
    await setField("Limit price", "4.61")
    await click("Submit paper order")
    expect(vi.mocked(api.submitOrder).mock.calls[0]![0]).toMatchObject({ limit_price: "4.61" })
    expect(host.textContent).toContain("INVALID_TICK: Limit price is not a positive multiple")
    expect(button("New order")).toBeDefined()
  })
  it("uses buttons and arrow keys for valid ticks and pads cents on blur", async () => {
    await render()
    await setField("Limit price", "3")
    await click("Decrease limit price one tick")
    expect(field("Limit price").value).toBe("2.95")
    await click("Increase limit price one tick")
    expect(field("Limit price").value).toBe("3.00")
    await act(async () => field("Limit price").dispatchEvent(new KeyboardEvent("keydown", { key: "ArrowUp", bubbles: true })))
    expect(field("Limit price").value).toBe("3.10")
    await act(async () => field("Limit price").dispatchEvent(new KeyboardEvent("keydown", { key: "ArrowDown", bubbles: true })))
    expect(field("Limit price").value).toBe("3.00")
    await setField("Limit price", "15.8")
    await act(async () => { field("Limit price").focus(); field("Limit price").blur() })
    expect(field("Limit price").value).toBe("15.80")
  })
  it("uses a server fee update in preference to the manual estimate", async () => {
    await render()
    await setField("Fee / contract", "1.25")
    await setField("Quantity", "3")
    expect(host.textContent).toContain("Estimated fees$3.75")
    await render({ ...trading, fee_per_contract: "0.65" })
    expect(host.querySelector('input[placeholder="Not provided by server"]')).toBeNull()
    expect(host.textContent).toContain("Estimated fees$1.95")
  })
  it("updates the session gate from live ticks and allows submission only in regular", async () => {
    vi.mocked(useLive).mockReturnValue(liveState(status, {
      type: "tick", engine: status.engine, feed: status.feed,
      underlyings: [{ ...status.underlyings[0]!, session: { name: "global", open: true, note: "Overnight" } }],
    }, "open"))
    await render()
    expect(host.textContent).toContain("SPX is in the overnight session")
    expect(button("Submit paper order").disabled).toBe(true)
    await click("Submit paper order")
    expect(api.submitOrder).not.toHaveBeenCalled()
    vi.mocked(useLive).mockReturnValue(liveState({ ...status, underlyings: [{ ...status.underlyings[0]!, session: { name: "regular", open: true, note: "Regular" } }] }, null, "open"))
    await render()
    expect(host.textContent).not.toContain("regular session only")
    expect(button("Submit paper order").disabled).toBe(false)
    await click("Submit paper order")
    expect(api.submitOrder).toHaveBeenCalledTimes(1)
  })
})
