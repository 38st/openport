// @vitest-environment jsdom
import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { act } from "react"
import { createRoot, type Root } from "react-dom/client"
import { afterEach, beforeEach, expect, it, vi } from "vitest"
import { liveState, useLive } from "../api/live"
import { api } from "../api/client"
import { risk, trading, status } from "../test/trading-fixtures"
import { LimitsEditor } from "./LimitsEditor"
vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
let host: HTMLDivElement
let root: Root
let client: QueryClient
beforeEach(() => {
  vi.mocked(useLive).mockReturnValue(liveState(status, null, "open"))
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  Object.defineProperty(HTMLDialogElement.prototype, "showModal", { configurable: true, value() { this.open = true } })
  Object.defineProperty(HTMLDialogElement.prototype, "close", { configurable: true, value() { this.open = false } })
  host = document.createElement("div"); document.body.append(host); root = createRoot(host)
  client = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity } } })
  vi.spyOn(api, "updateLimits").mockResolvedValue(risk)
})
afterEach(async () => {
  await act(async () => root.unmount()); host.remove(); client.clear()
  vi.restoreAllMocks(); vi.unstubAllGlobals()
})
async function click(text: string) {
  await act(async () => [...host.querySelectorAll("button")].find((b) => b.textContent === text)!.click())
}
async function change(label: string, value: string) {
  const input = host.querySelector<HTMLInputElement>(`[aria-label="${label}"]`)!
  await act(async () => {
    Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value")!.set!.call(input, value)
    input.dispatchEvent(new Event("input", { bubbles: true }))
  })
}
it("edits pending overrides, removes a row and validates a new symbol before saving", async () => {
  const initial = { ...risk, pending_limits: { ...risk.limits, underlying_overrides: { SPX: { dollar_delta: 10, vega: 20 } } } }
  await act(async () => root.render(<QueryClientProvider client={client}><LimitsEditor initial={initial} trading={trading} onClose={() => {}} /></QueryClientProvider>))
  expect(host.querySelector<HTMLInputElement>('[aria-label="Override 1 delta"]')!.value).toBe("10")
  await change("Override 1 delta", "100")
  await click("Add underlying override")
  await change("Override 2 symbol", "SPX")
  expect(host.querySelector<HTMLButtonElement>('button[type="submit"]')!.disabled).toBe(true)
  await change("Override 2 symbol", "QQQ")
  await change("Override 2 delta", "200")
  await change("Override 2 vega", "50")
  await act(async () => host.querySelector<HTMLButtonElement>('[aria-label="Remove override 1"]')!.click())
  await click("Save limits")
  expect(api.updateLimits).toHaveBeenCalledWith(risk.limits_revision, expect.objectContaining({ underlying_overrides: { QQQ: { dollar_delta: 200, vega: 50 } } }), trading.write)
})
