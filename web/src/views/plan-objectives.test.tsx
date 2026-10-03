// @vitest-environment jsdom
import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { act, type ReactNode } from "react"
import { createRoot, type Root } from "react-dom/client"
import { renderToStaticMarkup } from "react-dom/server"
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest"
import { api } from "../api/client"
import { liveState, useLive } from "../api/live"
import { tradingQueries } from "../api/trading"
import type { Account, Plan } from "../api/trading-types"
import { ResetDialog, planFacts } from "../components/ResetDialog"
import { evaluationBadge } from "../components/Sidebar"
import { ruleAlerts } from "../lib/rule-alerts"
import { account, plans, portfolio, risk, status, trades } from "../test/trading-fixtures"
import { DashboardView } from "./DashboardView"
import { RulesView, ruleText } from "./RulesView"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))

/** A static floor on the closed balance, with a plan daily loss limit, minimum days and consistency. */
const planned: Account = {
  ...account,
  rules: { ...account.rules, plan: "Custom plan", buy_only: false, drawdown_mode: "static", lock_at_start: false, profit_basis: "balance",
    daily_loss_limit: "2000.00", daily_loss_basis: "balance", daily_loss_action: "lock", consistency_percent: 40, consistency_basis: "total",
    min_trading_days: 3, min_profitable_days: 0, profitable_day_profit: null, day_end: "18:00" },
  evaluation: { ...account.evaluation, floor: "95000.00", floor_locked: true, drawdown_buffer: "5267.50", balance: "100150.00", profit_basis: "balance",
    target_remaining: "9850.00", decision_code: null,
    objectives: [
      { code: "PROFIT_TARGET", met: false, actual: 150, required: 10000, message: "$9850.00 to go to the target $110000.00 on the closed balance" },
      { code: "MIN_TRADING_DAYS", met: false, actual: 2, required: 3, message: "2 of 3 days with a trade; a day counts once one of your own orders, or a share trade, executes on it" },
      { code: "CONSISTENCY", met: false, actual: 66.7, required: 40, message: "The best day, $100.00 on 2026-09-22, is 67% of the total profit $150.00" },
    ],
    trading_days: 2, profitable_days: 1, best_day: { day: "2026-09-22", profit: "100.00" }, consistency_target: "250.00",
    daily_loss: { limit: "2000.00", basis: "balance", action: "lock", reference: "100150.00", level: "98150.00", room: "2117.50" },
    day_lock: null, day_locked_at: null, exit_equity: "100200.00", exit_cost: "67.50", liquidated_equity: null, liquidation_cost: null },
}
const locked: Account = { ...planned, evaluation: { ...planned.evaluation, day_lock: "DAILY_LOSS_LIMIT", day_locked_at: "2026-09-23T15:10:00Z",
  daily_loss: { ...planned.evaluation.daily_loss!, room: "0.00" } } }

const clients: QueryClient[] = []
function client(value: Account) {
  const c = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity, gcTime: Infinity } } })
  clients.push(c)
  const queries = tradingQueries(0, "17", true)
  c.setQueryData(queries.account.queryKey, value)
  c.setQueryData(queries.portfolio.queryKey, portfolio)
  c.setQueryData(queries.risk.queryKey, risk)
  c.setQueryData(queries.trades("current").queryKey, { account_version: "17", attempt: 2, trades })
  c.setQueryData(["plans"], { plans })
  return c
}
const render = (node: ReactNode, value: Account) => renderToStaticMarkup(<QueryClientProvider client={client(value)}>{node}</QueryClientProvider>)
let host: HTMLDivElement
let root: Root
beforeEach(() => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  vi.mocked(useLive).mockReturnValue(liveState(status, null, "open"))
})
afterEach(async () => {
  if (root) await act(async () => root.unmount())
  host?.remove()
  clients.splice(0).forEach((c) => c.clear())
  vi.restoreAllMocks(); vi.clearAllMocks(); vi.unstubAllGlobals()
})

describe("plan objectives in the terminal", () => {
  it("lists what a pass waits for, the daily loss room and what closing now would leave", () => {
    const html = render(<DashboardView />, planned)
    for (const text of ["Objectives to pass", "0 of 3 met", "Trading days", "2 of 3", "Consistency", "67% of 40% max",
      "Best day $100.00 on 2026-09-22", "needs $250.00 of profit", "Daily loss limit", "$98,150.00", "$2,117.50 room · locks the day",
      "to go on the closed balance", "Closed balance", "$100,150.00", "Equity if every position closed now", "$100,200.00", "· static"])
      expect(html).toContain(text)
    expect(html).not.toContain("Trading locked")
    // The default account shows none of it.
    const plain = render(<DashboardView />, account)
    for (const text of ["Objectives to pass", "Daily loss limit", "Closed balance", "if every position closed"]) expect(plain).not.toContain(text)
  })
  it("announces a locked day and a decision's code and liquidation cost", () => {
    const html = render(<DashboardView />, locked)
    expect(html).toContain("Trading locked until the next trading day")
    expect(html).toContain("6:00 pm ET")
    expect(html).toContain("Reached · locked until the next trading day")
    expect(renderToStaticMarkup(<>{evaluationBadge(locked)}</>)).toContain("locked today")
    const failed: Account = { ...planned, evaluation: { ...planned.evaluation, status: "failed", decision_code: "DAILY_LOSS_LIMIT",
      decided_at: "2026-09-23T15:10:00Z", decided_equity: "98100.00", decision: "Equity $98100.00 reached the daily loss limit",
      liquidated_equity: "98010.00", liquidation_cost: "90.00" },
      attempts: [{ ...account.attempts[0]!, status: "failed", decision_code: "DRAWDOWN_FLOOR" }] }
    const decided = render(<DashboardView />, failed)
    for (const text of ["Evaluation failed", "by the daily loss limit", "left $98,010.00, $90.00 less", "failed · drawdown floor"])
      expect(decided).toContain(text)
  })
  it("explains the plan's rules and offers its presets' objectives", () => {
    const texts = ruleText(planned, "0.65", "5000.00")
    const titles = texts.map((t) => t.title)
    expect(titles.slice(0, 4)).toEqual(["Profit target", "Static drawdown", "Plan daily loss limit", "Objectives to pass"])
    const bodies = texts.map((t) => renderToStaticMarkup(<>{t.body}</>))
    expect(bodies[0]).toContain("on the closed balance")
    expect(bodies[0]).toContain("The pass also waits for: at least 3 trading days; best day at most 40% of the total profit")
    expect(bodies[1]).toContain("It is static")
    expect(bodies[2]).toContain("$98,150.00")
    expect(bodies[2]).toContain("refuses opening orders until the next trading day")
    expect(bodies[3]).toContain("never fails the attempt")
    expect(bodies.join(" ")).toContain("A trading day ends at 6:00 pm ET")
    const presets: Plan[] = [...plans, { id: "static-50k", name: "Static 50K", summary: "Static floor.", initial_cash: "50000.00", unlocked_by: null,
      rules: { ...planned.rules, plan: "Static 50K", profit_target: "5000.00", max_drawdown: "4000.00", daily_loss_action: "fail" } }]
    const c = client(planned)
    c.setQueryData(["plans"], { plans: presets })
    const html = renderToStaticMarkup(<QueryClientProvider client={c}><RulesView /></QueryClientProvider>)
    for (const text of ["Never (static)", "$2,000 · fails", "Target, closed · At least 3 trading days · Best day at most 40% of the total profit"])
      expect(html).toContain(text)
    expect(planFacts(presets.at(-1)!)).toEqual(expect.arrayContaining(["$5,000 profit target on the closed balance",
      "$4,000 static drawdown: the floor stays at $46,000", "Trading day ends at 6:00 pm ET"]))
  })
  it.each([
    { room: "-53.90", equity: "99696.10", reached: "which equity has reached ($53.90 below it)" },
    { room: "0.00", equity: "99750.00", reached: "which equity has reached" },
  ])("explains a locked day's daily loss level with $room room", ({ room, equity, reached }) => {
    const value: Account = { ...locked,
      rules: { ...locked.rules, daily_loss_limit: "250.00", daily_loss_basis: "peak" },
      evaluation: { ...locked.evaluation, equity,
        daily_loss: { limit: "250.00", basis: "peak", action: "lock", reference: "100000.00", level: "99750.00", room } } }
    const page = document.createElement("div")
    page.innerHTML = render(<RulesView />, value)
    const text = page.textContent!.replace(/\s+/g, " ")
    expect(text).toContain(`Each trading day, equity may not touch $250.00 below the day's equity high: today $99,750.00, ${reached}.`)
    expect(text).toContain("Today is locked.")
    expect(text).not.toMatch(/−\$[\d,.]+ below current equity/)
    expect(text).not.toContain("below current equity")
  })
  it("keeps the daily loss level below current equity when room is positive", () => {
    const page = document.createElement("div")
    page.innerHTML = render(<RulesView />, planned)
    const text = page.textContent!.replace(/\s+/g, " ")
    expect(text).toContain("Each trading day, equity may not touch $2,000.00 below the day's opening balance: today $98,150.00, $2,117.50 below current equity.")
    expect(text).not.toContain("which equity has reached")
  })
  it("lowercases each objective after the first in the objectives sentence", () => {
    const value: Account = { ...planned, rules: { ...planned.rules, min_profitable_days: 2, profitable_day_profit: "100.00" } }
    const html = render(<RulesView />, value)
    expect(html).toContain("At least 3 trading days; at least 2 profitable days of $100+; best day at most 40% of the total profit.")
  })
  it("alerts on the plan's daily loss room, a locked day and a target waiting on objectives", () => {
    const near: Account = { ...planned, evaluation: { ...planned.evaluation, daily_loss: { ...planned.evaluation.daily_loss!, room: "400.00" } } }
    expect(ruleAlerts(near, risk).map((a) => a.id)).toEqual(expect.arrayContaining(["plan-loss-50", "plan-loss-25"]))
    expect(ruleAlerts(near, risk).map((a) => a.id)).not.toContain("plan-loss-10")
    expect(ruleAlerts(locked, risk).map((a) => a.id)).toContain("day-lock")
    expect(ruleAlerts(locked, risk).map((a) => a.id)).not.toContain("plan-loss-50")
    const reached: Account = { ...planned, evaluation: { ...planned.evaluation, objectives: [
      { code: "PROFIT_TARGET", met: false, actual: 10100, required: 10000, message: "The closed balance reached the target; close every position to count it" },
      { code: "MIN_TRADING_DAYS", met: true, actual: 3, required: 3, message: "3 of 3 days" }] } }
    const waits = ruleAlerts(reached, risk).find((a) => a.id === "target-waits")
    expect(waits?.body).toContain("close every position to count it")
  })
  it("submits a custom funded plan from the editor with consistency and buffer", async () => {
    Object.defineProperty(HTMLDialogElement.prototype, "showModal", { configurable: true, value() { this.open = true } })
    Object.defineProperty(HTMLDialogElement.prototype, "close", { configurable: true, value() { this.open = false } })
    vi.spyOn(api, "resetAccount").mockResolvedValue(account)
    host = document.createElement("div"); document.body.append(host); root = createRoot(host)
    await act(async () => root.render(<QueryClientProvider client={client(account)}>
      <ResetDialog trading={{ ...status.trading!, write: "open" }} attempt={2} onClose={() => {}} />
    </QueryClientProvider>))
    await act(async () => host.querySelector<HTMLInputElement>('input[value="custom"]')!.click())
    const set = async (name: string, value: string) => {
      const element = [...host.querySelectorAll("label")].find((l) => l.textContent?.startsWith(name))?.querySelector("input, select")
      if (!(element instanceof HTMLInputElement || element instanceof HTMLSelectElement)) throw new Error(`Missing field ${name}`)
      await act(async () => {
        Object.getOwnPropertyDescriptor(element instanceof HTMLInputElement ? HTMLInputElement.prototype : HTMLSelectElement.prototype, "value")!.set!.call(element, value)
        element.dispatchEvent(new Event(element instanceof HTMLInputElement ? "input" : "change", { bubbles: true }))
      })
    }
    await set("Phase", "funded")
    await set("Payout consistency percentages", "20, 25, 30")
    await set("Payout buffer", "2100")
    await set("Buffer payouts", "3")
    expect(host.textContent).toContain("20% / 25% / 30%")
    expect(host.textContent).toContain("first 3 payouts")
    await act(async () => host.querySelector("form")!.dispatchEvent(new Event("submit", { bubbles: true, cancelable: true })))
    expect(api.resetAccount).toHaveBeenCalledWith(expect.objectContaining({ rules: expect.objectContaining({ phase: "funded", profit_target: null,
      payouts: expect.objectContaining({ consistency_percents: [20, 25, 30], buffer: "2100", buffer_payouts: 3 }) }) }), "open")
  })
  it("starts a custom plan from a preset with the edited rules", async () => {
    Object.defineProperty(HTMLDialogElement.prototype, "showModal", { configurable: true, value() { this.open = true } })
    Object.defineProperty(HTMLDialogElement.prototype, "close", { configurable: true, value() { this.open = false } })
    vi.spyOn(api, "resetAccount").mockResolvedValue(account)
    host = document.createElement("div"); document.body.append(host); root = createRoot(host)
    await act(async () => root.render(<QueryClientProvider client={client(account)}>
      <ResetDialog trading={{ ...status.trading!, write: "open" }} attempt={2} initial="intraday-100k" onClose={() => {}} />
    </QueryClientProvider>))
    const radio = host.querySelector<HTMLInputElement>('input[value="custom"]')!
    await act(async () => radio.click())
    const set = async (name: string, value: string) => {
      const element = [...host.querySelectorAll("label")].find((l) => l.textContent?.startsWith(name))?.querySelector("input, select")
      if (!(element instanceof HTMLInputElement || element instanceof HTMLSelectElement)) throw new Error(`Missing field ${name}`)
      await act(async () => {
        Object.getOwnPropertyDescriptor(element instanceof HTMLInputElement ? HTMLInputElement.prototype : HTMLSelectElement.prototype, "value")!.set!.call(element, value)
        element.dispatchEvent(new Event(element instanceof HTMLInputElement ? "input" : "change", { bubbles: true }))
      })
    }
    await set("Plan name", "Two-step 100K")
    await set("Floor", "static")
    await set("Daily loss limit", "3000")
    await set("Reaching it", "fail")
    await set("Minimum trading days", "4")
    await set("Trading day ends", "18:00")
    expect(host.textContent).toContain("$3,000 daily loss limit from the day's opening equity: fails the attempt")
    await act(async () => host.querySelector("form")!.dispatchEvent(new Event("submit", { bubbles: true, cancelable: true })))
    expect(api.resetAccount).toHaveBeenCalledWith(expect.objectContaining({ initial_cash: "100000.00", reason: "Start Two-step 100K",
      rules: expect.objectContaining({ plan: "Two-step 100K", drawdown_mode: "static", daily_loss_limit: "3000", daily_loss_action: "fail",
        min_trading_days: 4, day_end: "18:00", profit_target: "10000.00", buy_only: true, payouts: null }) }), "open")
    // A bad value is named, and nothing is sent.
    vi.mocked(api.resetAccount).mockClear()
    await set("Minimum trading days", "x")
    expect(host.textContent).toContain("Minimum trading days must be a whole number from 0 to 366")
    await act(async () => host.querySelector("form")!.dispatchEvent(new Event("submit", { bubbles: true, cancelable: true })))
    expect(api.resetAccount).not.toHaveBeenCalled()
  })
})
