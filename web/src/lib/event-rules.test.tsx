// @vitest-environment jsdom
import { act, useState } from "react"
import { createRoot } from "react-dom/client"
import { describe, expect, it, vi } from "vitest"
import { api } from "../api/client"
import { PlanEditor, customPlan, planForm } from "../components/PlanEditor"
import type { AccountRules, EventWindow } from "../api/trading-types"
import { account } from "../test/trading-fixtures"
import { planEntryNotice, timeRuleFacts, timeRuleNotices } from "./plan-rules"
import { reasonEvidence } from "./orders"
vi.mock("../api/live", () => ({ useLive: () => ({ underlyings: [{ symbol: "SPY" }] }) }))
const rules: AccountRules = { ...account.rules, events: [{ kind: "news", time: "2026-09-22T14:00:00Z", symbol: "SPY", label: "CPI" }],
  news_before_minutes: 5, news_after_minutes: 10, news_action: "flatten", hold_restrictions: ["weekend", "earnings"], hold_cutoff: "15:45" }
const cutoff: EventWindow = { kind: "earnings", symbol: "SPY", label: "Results", start: "2026-09-22T19:45:00Z", end: "2026-09-22T21:00:00Z", active: true }
describe("F17/F59 calendar rules", () => {
  it("blocks scoped news openings at the inclusive start, allows reductions and exclusive end", () => {
    expect(planEntryNotice(rules, "SPY", "2026-09-22T13:54:59Z", false)).toBeNull()
    expect(planEntryNotice(rules, "SPY", "2026-09-22T13:55:00Z", false)).toContain("NEWS_BLACKOUT")
    expect(planEntryNotice(rules, "SPX", "2026-09-22T14:00:00Z", false)).toBeNull()
    expect(planEntryNotice(rules, "SPY", "2026-09-22T14:00:00Z", true)).toBeNull()
    expect(planEntryNotice(rules, "SPY", "2026-09-22T14:10:00Z", false)).toBeNull()
  })
  it("uses server calendar windows to block holdings and explain the next restriction", () => {
    const evaluation = { ...account.evaluation, next_event: cutoff, active_events: [cutoff] }
    expect(planEntryNotice(rules, "SPY", cutoff.start, false, evaluation)).toContain("HOLD_RESTRICTED")
    expect(planEntryNotice(rules, "SPY", cutoff.start, true, evaluation)).toBeNull()
    expect(planEntryNotice(rules, "SPX", cutoff.start, false, evaluation)).toBeNull()
    expect(planEntryNotice(rules, "SPY", cutoff.end, false, evaluation)).toBeNull()
    expect(timeRuleNotices(evaluation, rules).join(" ")).toContain("Active earnings holding cutoff")
    expect(timeRuleFacts(rules).join(" ")).toContain("5 minutes before / 10 after")
    expect(reasonEvidence({ code: "HOLD_RESTRICTED", message: "cutoff", limit: "15:45", scope: "earnings:SPY" })).toBe("Limit 15:45 · earnings:SPY")
  })
  it("round trips and validates editor fields", () => {
    const form = planForm({ initial_cash: "100000", rules })
    const result = customPlan(form, rules)
    expect(result).toMatchObject({ rules: { events: rules.events, news_action: "flatten", hold_cutoff: "15:45", hold_restrictions: rules.hold_restrictions } })
    for (const patch of [{ news_before_minutes: "241" }, { news_after_minutes: "1.5" }, { hold_cutoff: "17:00" },
      { events: [{ kind: "split" as const, time: "2026-09-22" }] },
      { events: [{ kind: "earnings" as const, time: "2026-02-30", symbol: "SPY" }] }])
      expect(customPlan({ ...form, ...patch }, rules)).toHaveProperty("error")
  })
  it("adds/removes rows and imports the daemon and known dividend calendars", async () => {
    vi.spyOn(api, "calendarEvents").mockResolvedValue({ events: [{ kind: "news", time: "2026-10-01T14:00:00Z", label: "FOMC" }] })
    vi.spyOn(api, "summary").mockResolvedValue({ symbol: "SPY", expiries: [{ dividends: [{ ex_date: "2026-10-02", amount: 1 }] }] } as Awaited<ReturnType<typeof api.summary>>)
    function Editor() { const [form, setForm] = useState(planForm({ initial_cash: "100000", rules: account.rules })); return <PlanEditor form={form} onChange={setForm} /> }
    vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
    const host = document.createElement("div"), root = createRoot(host)
    document.body.append(host)
    const click = async (text: string) => {
      const button = [...host.querySelectorAll("button")].find((b) => b.textContent === text)
      expect(button).toBeDefined()
      await act(async () => { button!.click() })
    }
    try {
      await act(async () => root.render(<Editor />))
      await click("Add event")
      expect(host.textContent).toContain("Event 1 kind")
      await click("Remove event 1")
      await click("Import server calendar")
      expect([...host.querySelectorAll("input")].some((e) => e.value === "FOMC")).toBe(true)
      await click("Import known dividend ex-dates")
      expect([...host.querySelectorAll("input")].some((e) => e.value === "Dividend ex-date")).toBe(true)
      expect([...host.querySelectorAll("input")].some((e) => e.value === "2026-10-02")).toBe(true)
    } finally { await act(async () => root.unmount()); host.remove(); vi.restoreAllMocks(); vi.unstubAllGlobals() }

  })
})
