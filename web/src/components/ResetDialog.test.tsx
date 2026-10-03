// @vitest-environment jsdom
import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { act } from "react"
import { createRoot, type Root } from "react-dom/client"
import { afterEach, beforeEach, expect, it, vi } from "vitest"
import { api } from "../api/client"
import { liveState, useLive } from "../api/live"
import { tradingQueries } from "../api/trading"
import { account, plans, status } from "../test/trading-fixtures"
import { ResetDialog } from "./ResetDialog"
import { NewAccountDialog } from "./AccountSwitcher"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
let host: HTMLDivElement
let root: Root
let client: QueryClient
beforeEach(() => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  Object.defineProperty(HTMLDialogElement.prototype, "showModal", { configurable: true, value() { this.open = true } })
  Object.defineProperty(HTMLDialogElement.prototype, "close", { configurable: true, value() { this.open = false } })
  host = document.createElement("div"); document.body.append(host); root = createRoot(host)
  client = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity } } })
  client.setQueryData(["plans"], { plans })
  client.setQueryData(tradingQueries(0, "17", true).account.queryKey, account)
  vi.mocked(useLive).mockReturnValue(liveState(status, null, "open"))
  vi.spyOn(api, "resetAccount").mockResolvedValue(account)
})
it.each(["reset", "create"])("submits itemized fees when starting an account attempt (%s)", async (kind) => {
  vi.spyOn(api, "createAccount").mockResolvedValue({ account: { id: "fees", name: "Fees", account_version: "1", plan: "Practice", equity: "100000.00" } })
  const plan = plans.find((entry) => entry.id === "intraday-100k")!
  await act(async () => root.render(<QueryClientProvider client={client}>
    {kind === "reset" ? <ResetDialog trading={{ ...status.trading!, write: "open" }} attempt={2} initial={plan.id} onClose={() => {}} />
      : <NewAccountDialog trading={{ ...status.trading!, write: "open" }} onClose={() => {}} onCreated={() => {}} />}
  </QueryClientProvider>))
  if (kind === "create") {
    await act(async () => host.querySelector<HTMLInputElement>(`input[value="${plan.id}"]`)!.click())
  }
  const select = host.querySelector<HTMLSelectElement>('select[aria-label="Fees"]')!
  expect(select.value).toBe("flat")
  await act(async () => { select.value = "itemized"; select.dispatchEvent(new Event("change", { bubbles: true })) })
  expect(host.textContent).toContain("$10 per leg per order")
  expect(host.textContent).toContain("$5 per contract")
  await act(async () => host.querySelector("form")!.dispatchEvent(new Event("submit", { bubbles: true, cancelable: true })))
  expect(kind === "reset" ? api.resetAccount : api.createAccount).toHaveBeenCalledWith(
    expect.objectContaining({ plan: plan.id, fee_model: "itemized" }), "open")
})
afterEach(async () => {
  await act(async () => root.unmount()); host.remove(); client.clear()
  vi.restoreAllMocks(); vi.clearAllMocks(); vi.unstubAllGlobals()
})
it.each([false, true])("submits the selected fill preset, preserving the default request (%s)", async (conservative) => {
  const plan = plans.find((entry) => entry.id === "intraday-100k")!
  await act(async () => root.render(<QueryClientProvider client={client}>
    <ResetDialog trading={{ ...status.trading!, write: "open" }} attempt={2} initial={plan.id} onClose={() => {}} />
  </QueryClientProvider>))
  const select = host.querySelector("select")!
  expect(select.value).toBe("as_displayed")
  if (conservative) {
    await act(async () => { select.value = "conservative"; select.dispatchEvent(new Event("change", { bubbles: true })) })
    expect(host.textContent).toContain("wait 1,000 ms on market time")
    expect(host.textContent).toContain("1 extra tick")
  }
  await act(async () => host.querySelector("form")!.dispatchEvent(new Event("submit", { bubbles: true, cancelable: true })))
  expect(api.resetAccount).toHaveBeenCalledWith({ plan: plan.id, reason: `Start ${plan.name}`,
    ...(conservative ? { fill_model: "conservative" } : {}) }, "open")
})
