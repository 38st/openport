// @vitest-environment jsdom
import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { act } from "react"
import { createRoot, type Root } from "react-dom/client"
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest"
import { liveState, useLive } from "../api/live"
import type { Position } from "../api/trading-types"
import { portfolio, status } from "../test/trading-fixtures"
import { AbandonDialog, ExerciseInstructionDialog, SettleDialog } from "./StockActions"

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

describe("disposing of a worthless position", () => {
  it("forfeits cash settlement for an instructed index long", async () => {
    const long = { ...portfolio.positions[0]!, do_not_exercise: true, awaiting_settlement: true } as Position
    await act(async () => root.render(<QueryClientProvider client={new QueryClient()}>
      <ExerciseInstructionDialog position={long} trading={status.trading!} onClose={() => {}} /></QueryClientProvider>))
    expect(host.textContent).toContain("settles in cash at intrinsic value")
    await act(async () => root.render(<QueryClientProvider client={new QueryClient()}>
      <SettleDialog position={long} trading={status.trading!} onClose={() => {}} /></QueryClientProvider>))
    await type(host.querySelector("input")!, String(long.strike + 100))
    expect(host.textContent).toContain("do-not-exercise instruction closes this long at zero")
    expect(host.textContent).toContain("Settlement value $0.00")
    expect(host.textContent).not.toContain("delivering")
  })
  it("abandons a long nobody bids for at zero", async () => {
    const long = { ...portfolio.positions[0]!, no_bid: true, mark: "0.03", market_value: "6.00" } as Position
    const fetcher = vi.fn(async () => new Response(JSON.stringify({ ...portfolio, positions: [] }), { status: 200 }))
    vi.stubGlobal("fetch", fetcher)
    const closed = vi.fn()
    await act(async () => root.render(<QueryClientProvider client={new QueryClient()}>
      <AbandonDialog position={long} trading={status.trading!} onClose={closed} /></QueryClientProvider>))
    expect(host.textContent).toContain("2 contracts leave the account at $0.00, without a fee, since nobody bids for them.")
    expect(host.textContent).toContain("$920.00")
    const abandon = [...host.querySelectorAll("button")].find((b) => b.textContent === "Abandon 2")!
    await act(async () => abandon.click())
    expect(fetcher).toHaveBeenCalledWith("/api/positions/abandon", expect.objectContaining({ method: "POST",
      body: JSON.stringify({ symbol: long.symbol }) }))
    expect(closed).toHaveBeenCalled()
  })

  it("warns that an expired long in the money forfeits its settlement", async () => {
    const expired = { ...portfolio.positions[0]!, awaiting_settlement: true, no_bid: true, strike: 6800 } as Position
    vi.mocked(useLive).mockReturnValue({ ...liveState(status, null, "open"),
      underlyings: [{ ...status.underlyings[0]!, symbol: "SPX", spot: 6810 }] } as ReturnType<typeof useLive>)
    vi.stubGlobal("fetch", vi.fn())
    await act(async () => root.render(<QueryClientProvider client={new QueryClient()}>
      <AbandonDialog position={expired} trading={status.trading!} onClose={() => {}} /></QueryClientProvider>))
    expect(host.textContent).toContain("instead of waiting for the settlement")
    expect(host.textContent).toContain("it is $10.00 in the money: the settlement may still pay $2,000.00")
  })

  it("gives and withdraws a do-not-exercise instruction", async () => {
    const call = { ...portfolio.positions[0]!, symbol: "SPY   261022C00500000", underlying: "SPY", strike: 500, do_not_exercise: false } as Position
    const fetcher = vi.fn(async () => new Response(JSON.stringify(portfolio), { status: 200 }))
    vi.stubGlobal("fetch", fetcher)
    await act(async () => root.render(<QueryClientProvider client={new QueryClient()}>
      <ExerciseInstructionDialog position={call} trading={status.trading!} onClose={() => {}} /></QueryClientProvider>))
    expect(host.textContent).toContain("expire worthless however far in the money, and no SPY shares change hands")
    await act(async () => [...host.querySelectorAll("button")].find((b) => b.textContent === "Do not exercise")!.click())
    expect(fetcher).toHaveBeenCalledWith("/api/positions/instruction", expect.objectContaining({ method: "POST",
      body: JSON.stringify({ symbol: call.symbol, do_not_exercise: true }) }))
    await act(async () => root.render(<QueryClientProvider client={new QueryClient()}>
      <ExerciseInstructionDialog position={{ ...call, do_not_exercise: true }} trading={status.trading!} onClose={() => {}} /></QueryClientProvider>))
    expect(host.textContent).toContain("Withdraw the instruction")
    expect(host.textContent).toContain("buy 200 SPY shares")
    await act(async () => [...host.querySelectorAll("button")].find((b) => b.textContent === "Exercise at expiry")!.click())
    expect(fetcher).toHaveBeenLastCalledWith("/api/positions/instruction", expect.objectContaining({
      body: JSON.stringify({ symbol: call.symbol, do_not_exercise: false }) }))
  })
})
