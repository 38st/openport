// @vitest-environment jsdom
import { act } from "react"
import { createRoot, type Root } from "react-dom/client"
import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest"
import { api } from "../api/client"
import { liveState, useLive } from "../api/live"
import { status } from "../test/trading-fixtures"
import type { NotificationStatus } from "../api/types"
import { NotificationSettings } from "./NotificationSettings"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
let root: Root
let host: HTMLDivElement
let client: QueryClient
const notifications: NotificationStatus = {
  enabled: true, queue_depth: 1, queue_capacity: 256, dropped: 2,
  channels: [{ id: "phone", type: "telegram", enabled: true, events: ["fill", "floor"], floor_distance: "500.00",
    delivered: 3, failures: 1, dropped: 2, last_attempt: "2026-09-22T14:00:00Z", last_delivery: null, last_error: "HTTP_429" }],
}
function live(source: "live" | "replay" = "live", configured: NotificationStatus | undefined = notifications, mode: "open" | "disabled" = "open") {
  const state = liveState({ ...status, notifications: configured }, null, "open", 0, "main", () => {}, source)
  vi.mocked(useLive).mockReturnValue({ ...state, trading: state.trading ? { ...state.trading, write: mode } : { enabled: true, reason: null, account_version: "1", write: mode, kill_latched: false, fee_per_contract: "0.65", initial_cash: "100000.00" } })
}
async function render() { await act(async () => root.render(<QueryClientProvider client={client}><NotificationSettings /></QueryClientProvider>)) }
function button(text: string) { return [...host.querySelectorAll("button")].find((item) => item.textContent === text)! }
beforeEach(() => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  host = document.createElement("div"); document.body.append(host); root = createRoot(host)
  client = new QueryClient({ defaultOptions: { queries: { retry: false } } })
  vi.spyOn(api, "testNotification").mockResolvedValue({ queued: true })
  vi.spyOn(api, "configureNotification").mockResolvedValue(notifications)
  live()
})
afterEach(async () => { await act(async () => root.unmount()); host.remove(); client.clear(); vi.restoreAllMocks(); vi.unstubAllGlobals() })

describe("notification settings", () => {
  it("shows channel filters, delivery failures and queue counts without credential inputs", async () => {
    await render()
    expect(host.textContent).toContain("phone")
    expect(host.textContent).toContain("telegram")
    expect(host.textContent).toContain("HTTP_429")
    expect(host.textContent).toContain("3 delivered · 1 failed attempts · 2 dropped")
    expect(host.textContent).toContain("Queue: 1/256")
    expect(host.querySelectorAll('input[type="checkbox"]')).toHaveLength(9)
    expect(host.querySelector('input[type="password"]')).toBeNull()
    expect(button("Save filters").disabled).toBe(true)
  })
  it("queues a test and distinguishes queued from delivered", async () => {
    await render()
    await act(async () => button("Test phone").click())
    expect(api.testNotification).toHaveBeenCalledWith("phone", "open")
    expect(host.textContent).toContain("Test queued")
    expect(host.textContent).toContain("Last delivery: none")
  })
  it("saves only nonsecret settings", async () => {
    await render()
    const checkbox = [...host.querySelectorAll<HTMLInputElement>('input[type="checkbox"]')].find((input) => input.parentElement?.textContent?.includes("Assignments"))!
    await act(async () => checkbox.click())
    expect(button("Save filters").disabled).toBe(false)
    await act(async () => button("Save filters").click())
    expect(api.configureNotification).toHaveBeenCalledWith("phone", { enabled: true, events: ["fill", "floor", "assignment"], floor_distance: "500.00" }, "open")
    expect(host.textContent).toContain("Filters saved for this server session")
  })
  it("does not display provider exceptions or claim a failed test delivered", async () => {
    vi.mocked(api.testNotification).mockRejectedValue(new Error("https://secret-token"))
    await render(); await act(async () => button("Test phone").click())
    expect(host.querySelector('[role="alert"]')?.textContent).toContain("Could not queue")
    expect(host.textContent).not.toContain("secret-token")
  })
  it("hides actions on replays", async () => {
    live("replay"); await render()
    expect(host.textContent).toContain("Switch to live")
    expect(host.querySelector("button")).toBeNull()
    expect(api.testNotification).not.toHaveBeenCalled()
  })
  it("explains server configuration when no channels exist or the server is older", async () => {
    live("live", { ...notifications, channels: [] }); await render()
    expect(host.textContent).toContain("No channels configured")
    expect(host.textContent).toContain("OPENPORT_NOTIFY_JSON")
    vi.mocked(useLive).mockReturnValue(liveState(status, null, "open")); await render()
    expect(host.textContent).toContain("No channels configured")
  })
  it("disables writes for read-only settings and tests for disabled channels", async () => {
    live("live", notifications, "disabled"); await render()
    expect(button("Test phone").disabled).toBe(true)
    live("live", { ...notifications, channels: [{ ...notifications.channels[0]!, enabled: false }] }); await render()
    expect(button("Test phone").disabled).toBe(true)
  })
  it("prevents duplicate test clicks while the request is pending", async () => {
    let finish: (value: { queued: boolean }) => void = () => {}
    vi.mocked(api.testNotification).mockReturnValue(new Promise((resolve) => { finish = resolve }))
    await render(); await act(async () => button("Test phone").click())
    expect(button("Test phone").disabled).toBe(true)
    await act(async () => finish({ queued: true }))
    expect(button("Test phone").disabled).toBe(false)
  })
})
