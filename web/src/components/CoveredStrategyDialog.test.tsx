// @vitest-environment jsdom
import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { act } from "react"
import { createRoot, type Root } from "react-dom/client"
import { afterEach, beforeEach, expect, it, vi } from "vitest"
import { api } from "../api/client"
import { liveState, useLive } from "../api/live"
import { account, chain, order, portfolio, status } from "../test/trading-fixtures"
import { waitForRender } from "../test/render"
import { CoveredStrategyDialog } from "./CoveredStrategyDialog"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
vi.mock("./OrderPreview", () => ({ useOrderPreview: () => ({}), OrderPreviewPanel: () => null }))
vi.mock("./OrderTicket", () => ({ OrderResult: () => <p>Option order result</p> }))
let root: Root, host: HTMLDivElement
const etf = { ...chain, symbol: "SPY", spot: 510, strikes: chain.strikes.map((r) => ({ ...r, strike: 510,
  call: { ...r.call!, symbol: "SPY   261016C00510000" }, put: { ...r.put!, symbol: "SPY   261016P00510000" },
})) }
beforeEach(() => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  vi.mocked(useLive).mockReturnValue(liveState(status, null, "open"))
  vi.spyOn(api, "account").mockResolvedValue({ ...account, rules: { ...account.rules, buy_only: false, defined_risk: false } })
  vi.spyOn(api, "previewStock").mockImplementation(async () => { throw new Error("preview unavailable") })
  vi.spyOn(api, "tradeStock").mockResolvedValue(portfolio)
  vi.spyOn(api, "submitOrder").mockResolvedValue({ account_version: "18", order, fills: [] })
  Object.defineProperty(HTMLDialogElement.prototype, "showModal", { configurable: true, value() { this.open = true } })
  Object.defineProperty(HTMLDialogElement.prototype, "close", { configurable: true, value() { this.open = false } })
  host = document.createElement("div"); document.body.append(host); root = createRoot(host)
})
afterEach(async () => { await act(async () => root.unmount()); host.remove(); vi.restoreAllMocks(); vi.unstubAllGlobals() })
const button = (text: string) => [...host.querySelectorAll("button")].find((b) => b.textContent === text)!
it.each(["covered-call", "collar"] as const)("blocks an unprotected %s before buying shares", async (kind) => {
  vi.mocked(api.account).mockResolvedValue({ ...account, rules: { ...account.rules, buy_only: false, defined_risk: false, require_stop_loss: true } })
  await act(async () => root.render(<QueryClientProvider client={new QueryClient()}><CoveredStrategyDialog kind={kind} chain={etf} trading={status.trading!} onClose={() => {}} /></QueryClientProvider>))
  await waitForRender(() => expect(host.textContent).toContain("Stop-loss required by this plan"))
  expect(button("1. Buy 100 shares").disabled).toBe(true)
  await act(async () => button("1. Buy 100 shares").click())
  expect(api.tradeStock).not.toHaveBeenCalled()
})
it.each(["covered-call", "collar"] as const)("buys shares once before sending %s options", async (kind) => {
  await act(async () => root.render(<QueryClientProvider client={new QueryClient()}><CoveredStrategyDialog kind={kind} chain={etf} trading={status.trading!} onClose={() => {}} /></QueryClientProvider>))
  expect(host.textContent).toContain("not atomic")
  expect(api.submitOrder).not.toHaveBeenCalled()
  await act(async () => button("1. Buy 100 shares").click())
  expect(api.tradeStock).toHaveBeenCalledWith("SPY", "buy", 100, "open")
  expect(host.textContent).toContain("Bought 100 shares")
  await act(async () => button("2. Send options at market").click())
  expect(api.tradeStock).toHaveBeenCalledTimes(1)
  const sent = vi.mocked(api.submitOrder).mock.calls[0]![0]
  expect(sent).toMatchObject({ quantity: 1, type: "market", time_in_force: "ioc", tags: [kind] })
  if (kind === "covered-call") expect(sent).toMatchObject({ symbol: etf.strikes[0]!.call!.symbol, side: "sell" })
  else expect(sent.legs).toEqual([{ symbol: etf.strikes[0]!.call!.symbol, side: "sell", ratio: 1 }, { symbol: etf.strikes[0]!.put!.symbol, side: "buy", ratio: 1 }])
})
it("does not rebuy shares when the option request fails", async () => {
  vi.mocked(api.submitOrder).mockRejectedValueOnce(new Error("option rejected"))
  await act(async () => root.render(<QueryClientProvider client={new QueryClient()}><CoveredStrategyDialog kind="covered-call" chain={etf} trading={status.trading!} onClose={() => {}} /></QueryClientProvider>))
  await act(async () => button("1. Buy 100 shares").click())
  await act(async () => button("2. Send options at market").click())
  await act(async () => button("2. Send options at market").click())
  expect(api.tradeStock).toHaveBeenCalledTimes(1)
  expect(vi.mocked(api.submitOrder).mock.calls[0]![0]).toEqual(vi.mocked(api.submitOrder).mock.calls[1]![0])
})
