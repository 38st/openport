import type { Volatility } from "../api/types"
import { account, risk, summary } from "./trading-fixtures"

export const briefTime = "2026-09-23T12:00:00Z"
const point = { strike: null, vol: 20, reason: null }
const skew = { call: point, put: point, rr: -2, bf: .5 }
export const sessionMove = (date: string, percent: number | null = 1) => ({ date, percent, points: percent == null ? null : percent * 70,
  forward: 7000, shared: true, calendar_arbitrage: false, proxy: false, truncated: false, label: null, reason: percent == null ? "no_covering_expiry" : null })
export const briefVolatility: Volatility = {
  symbol: "SPX", as_of: briefTime, version: 1, spot: 7000, forward: 7010, units: "vol_points",
  mfiv: { expiries: [], constant: [{ days: 30, variance: .04, vol: 20, near: "2026-10-16PM", next: "2026-10-30PM", truncated: true, proxy: false, reason: null }] },
  atm: { expiries: [], constant: [{ days: 30, variance: .0361, vol: 19, near: "2026-10-16PM", next: "2026-10-30PM", truncated: false, proxy: true, reason: null }] },
  skew: { expiries: [], days: 30, delta25: skew, delta10: skew, proxy: false, reason: null, delta_convention: "Black-76 forward delta" },
  term: { mfiv9_30: 1.1, mfiv30_93: .95, atm30_7: 1, truncated: true, proxy: false },
  realized: { daily_as_of: "2026-09-22", windows: [10, 21].map(sessions => ({ sessions,
    close_to_close: { vol: 16, fallback: false, reason: null }, parkinson: { vol: 17, fallback: false, reason: null },
    garman_klass: { vol: 17, fallback: false, reason: null }, yang_zhang: { vol: 17, fallback: false, reason: null } })), cones: [], intraday: [], today: null },
  vrp: { spread: 4, ratio: 1.25, truncated: true, proxy: false, reason: null },
  implied_moves: { intervals: [], sessions: [sessionMove("2026-09-23"), sessionMove("2026-09-24"), sessionMove("2026-09-25"), sessionMove("2026-09-28")],
    today_points: null, today_percent: null, today_reason: "no_same_day_expiry", today_proxy: false, today_truncated: false, today_calendar_arbitrage: false },
  iv_rank: .6, iv_percentile: .7, history_sessions: 42, history_basis: { current: "own_mfiv", own_sessions: 22, proxy_sessions: 20, window: 252 },
  proxy: { name: "VIX", used: true, start: "2026-08-01" },
  sources: { mfiv: "Quote strip", atm: "Smile", skew: "Forward delta", term: "MFIV ratios", realized: "Daily closes", vrp: "Ex ante", implied_moves: "Session allocation" },
}
export const briefSummary = { ...summary, as_of: briefTime, spot: 7070, spot_source: "parity" as const,
  exposure: { ...summary.exposure, gamma_flip: 6950, call_wall: 7100, put_wall: 6900 } }
export const briefAccount = { ...account, time: briefTime,
  guardrails: { soft_floor: "96000", soft_floor_percent: 0, max_opening_trades: 5, cooldown_loss: "100", cooldown_minutes: 10, profit_lock: "1000" },
  guardrail_state: { soft_floor: "96000", opening_trades: 1, latched: [], cooldown_until: null, cooldown_seconds: 0 } }
export const briefRisk = { ...risk, complete: true, guardrails: briefAccount.guardrails, guardrail_state: briefAccount.guardrail_state }
