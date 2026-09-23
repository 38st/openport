import { describe, expect, it } from "vitest"
import type { Account, PayoutStatus } from "../api/trading-types"
import { account, fundedAccount, plans } from "../test/trading-fixtures"
import { cycleDays, lockReason, payoutAmountError, payoutCap, payoutChecks, unlockedFundedPlan } from "./payouts"
import { compareMoney, percentOfMoney, sumMoney } from "./trading"

const passed: Account = { ...account, evaluation: { ...account.evaluation, status: "passed" } }
const status = fundedAccount.payout as PayoutStatus

describe("payouts", () => {
  it("compares, sums and takes percentages of decimal strings exactly", () => {
    expect(compareMoney("10.10", "10.1")).toBe(0)
    expect(compareMoney("0.01", "0.009999")).toBe(1)
    expect(compareMoney("-5", "0")).toBe(-1)
    expect(compareMoney("x", "0")).toBeNull()
    expect(sumMoney(["0.10", "0.20", "1600.00", "0.000001"])).toBe("1600.300001")
    expect(sumMoney([])).toBe("0.00")
    expect(percentOfMoney("46.75", 80)).toBe("37.4000")
    expect(percentOfMoney("0.01", 50)).toBe("0.0050")
  })
  it("caps each payout number, repeating the last cap", () => {
    const caps = ["2000.00", "3000.00", "6000.00"]
    expect(payoutCap(caps, 1)).toBe("2000.00")
    expect(payoutCap(caps, 3)).toBe("6000.00")
    expect(payoutCap(caps, 9)).toBe("6000.00")
    expect(payoutCap([], 1)).toBeNull()
  })
  it("unlocks a funded plan only after passing the evaluation it names", () => {
    const funded = plans[2]!
    expect(lockReason(plans[1]!, plans, account)).toBeNull()
    expect(lockReason(funded, plans, account)).toBe("Pass Intraday 100K to unlock")
    expect(lockReason(funded, plans, undefined)).toBe("Pass Intraday 100K to unlock")
    expect(lockReason(funded, plans, passed)).toBeNull()
    expect(lockReason(funded, plans, { ...passed, rules: { ...passed.rules, plan: "Intraday 25K" } })).not.toBeNull()
    expect(unlockedFundedPlan(plans, passed)?.id).toBe("funded-intraday-100k")
    expect(unlockedFundedPlan(plans, account)).toBeNull()
  })
  it("counts a payout cycle from the trading day of the request", () => {
    expect(cycleDays(fundedAccount).map((d) => d.day)).toEqual(["2026-09-22", "2026-09-23", "2026-09-24"])
    const later = { ...fundedAccount, evaluation: { ...fundedAccount.evaluation,
      payouts: [...fundedAccount.evaluation.payouts, { ...fundedAccount.evaluation.payouts[0]!, number: 2, day: "2026-09-24" }] } }
    expect(cycleDays(later).map((d) => d.day)).toEqual(["2026-09-24"])
    expect(cycleDays(account)).toEqual(account.evaluation.days)
  })
  it("lists every requirement with its standing", () => {
    expect(payoutChecks(status)).toEqual([
      { label: "Funded account is active", ok: true },
      { label: "No open positions or working orders", ok: true },
      { label: "Qualifying days of $200+ net realised profit", ok: false, value: "3 of 8" },
      { label: "Available payout meets the minimum", ok: true, value: "$3,000.00 of $1,000.00" },
    ])
    expect(payoutChecks({ ...status, maximum: "0.00" }).at(-1)?.ok).toBe(false)
    expect(payoutChecks({ ...status, maximum: "999.99" }).at(-1)?.ok).toBe(false)
    expect(payoutChecks({ ...status, active: false, flat: false }).slice(0, 2).map((c) => c.ok)).toEqual([false, false])
  })
  it("validates a whole-cent amount within the range", () => {
    expect(payoutAmountError("1500", status)).toBeNull()
    expect(payoutAmountError("3000.00", status)).toBeNull()
    expect(payoutAmountError("1000.001", status)).toBe("Enter an amount in dollars and cents")
    expect(payoutAmountError("-5", status)).toBe("Enter an amount in dollars and cents")
    expect(payoutAmountError("0", status)).toBe("Enter an amount above zero")
    expect(payoutAmountError("999.99", status)).toBe("The minimum payout is $1,000.00")
    expect(payoutAmountError("3000.01", status)).toBe("The most you can request now is $3,000.00")
  })
})
