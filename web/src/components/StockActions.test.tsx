// @vitest-environment jsdom
import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { act } from "react"
import { createRoot, type Root } from "react-dom/client"
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest"
import { liveState, useLive } from "../api/live"
import type { Position } from "../api/trading-types"
import { portfolio, status } from "../test/trading-fixtures"
import { SettleDialog } from "./StockActions"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))

let root: Root
let host: HTMLDivElement
beforeEach(() => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  Object.defineProperty(HTMLDialogElement.prototype, "showModal", { configurable: true, value() { this.open = true } })
  Object.defineProperty(HTMLDialogElement.prototype, "close", { configurable: true, value() { this.open = false } })
  vi.mocked(useLive).mockReturnValue(liveState(status, null, "open"))
  host = document.createElement("div")
  document.body.append(host)
  root = createRoot(host)
})
afterEach(async () => {
  await act(async () => root.unmount())
  host.remove()
  vi.unstubAllGlobals()
})
const type = async (input: HTMLInputElement, value: string) => {
  const setter = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value")!.set!
  await act(async () => { setter.call(input, value); input.dispatchEvent(new Event("input", { bubbles: true })) })
}

describe("settling by hand", () => {
  it("explains why, previews intrinsic value and posts the official value", async () => {
    const expired = { ...portfolio.positions[1]!, settle_by: "manual" } as Position  // a short SPX 6800 put, AM-settled
    const fetcher = vi.fn(async () => new Response(JSON.stringify({ account_version: "18", position_closed: true }), { status: 200 }))
    vi.stubGlobal("fetch", fetcher)
    const closed = vi.fn()
    await act(async () => root.render(<QueryClientProvider client={new QueryClient()}>
      <SettleDialog position={expired} trading={status.trading!} onClose={closed} /></QueryClientProvider>))
    expect(host.textContent).toContain("special opening quotation")
    const settle = [...host.querySelectorAll("button")].find((b) => b.textContent === "Settle")!
    expect(settle.disabled).toBe(true)
    await type(host.querySelector("input")!, "6790.50")
    expect(host.textContent).toContain("Intrinsic value $9.50 a share, −$950.00 for 1 short contract.")
    await act(async () => settle.click())
    expect(fetcher).toHaveBeenCalledWith("/api/settlements", expect.objectContaining({ method: "POST",
      body: JSON.stringify({ symbol: expired.symbol, value: "6790.50" }) }))
    expect(closed).toHaveBeenCalled()
  })
})
