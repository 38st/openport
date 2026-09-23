// @vitest-environment jsdom
import { act } from "react"
import { createRoot, type Root } from "react-dom/client"
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest"
import { liveState, useLive } from "../api/live"
import { status } from "../test/trading-fixtures"
import { createWelcomeStore, Welcome, welcome } from "./Welcome"

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
  welcome.close()
  vi.unstubAllGlobals()
})

describe("the welcome", () => {
  it("shows once per browser, and again when asked", () => {
    const items = new Map<string, string>()
    const store = createWelcomeStore(() => ({ getItem: (k) => items.get(k) ?? null, setItem: (k, v) => { items.set(k, v) } }))
    expect(store.get()).toBe(true)
    store.close()
    expect(store.get()).toBe(false)
    expect(items.get("openport.welcome")).toBe("seen")
    expect(createWelcomeStore(() => ({ getItem: (k) => items.get(k) ?? null, setItem: () => {} })).get()).toBe(false)
    store.show()
    expect(store.get()).toBe(true)
    // Without storage it waits to be asked rather than returning on every visit.
    expect(createWelcomeStore(() => { throw new Error("blocked") }).get()).toBe(false)
  })

  it("says where the data comes from and starts on the chain", async () => {
    const navigate = vi.fn()
    welcome.show()
    await act(async () => root.render(<Welcome onNavigate={navigate} />))
    const text = host.textContent ?? ""
    expect(text).toContain("Welcome to openport")
    expect(text).toContain(`${status.provider.name}, `)
    expect(text).toContain("nothing reaches an exchange")
    expect(text).toContain("not investment advice")
    const start = [...host.querySelectorAll("button")].find((b) => b.textContent === "Start on the chain")!
    await act(async () => start.click())
    expect(navigate).toHaveBeenCalledWith("chain")
    expect(welcome.get()).toBe(false)
    expect(host.textContent).toBe("")
  })
})
