import type { Surface, SurfaceExpiry } from "../api/types"
import { expiryLabel, fixed } from "../lib/format"
import { smileColor } from "../lib/svi"

export function SviTable({ expiries, violations }: { expiries: SurfaceExpiry[]; violations: NonNullable<Surface["calendar_violations"]> }) {
  if (!expiries.length) return null
  return (
    <div className="mt-4 border-t border-border pt-3">
      <h3 className="mb-2 text-xs font-medium text-muted">SVI calibration · RMSE in vol points</h3>
      <table className="w-full table-fixed text-[11px]" aria-label="SVI parameters and arbitrage diagnostics">
        <thead className="text-muted">
          <tr>
            <th className="w-[27%] pb-1 text-left font-normal">Expiry / checks</th>
            <th className="w-[52%] pb-1 text-left font-normal">a · b · ρ · m · σ</th>
            <th className="pb-1 text-right font-normal">RMSE / points</th>
          </tr>
        </thead>
        <tbody>
          {expiries.map((e, i) => {
            const fit = e.svi
            const pairs = violations.filter((p) => p.earlier === e.id || p.later === e.id)
            return (
              <tr key={e.id} className="border-t border-border/50 align-top">
                <td className="py-2 pr-2">
                  <span className="break-words" style={{ color: smileColor(i) }}>{expiryLabel(e.id, true)}</span>
                  <div className="mt-1 flex flex-wrap gap-1">
                    {fit && !fit.butterfly_ok && <span className="rounded border border-danger px-1 text-danger" title={`min g ${fixed(fit.butterfly_min_g, 5)} at k ${fixed(fit.butterfly_k, 4)}`}>Butterfly violation</span>}
                    {pairs.length > 0 && <span className="rounded border border-danger px-1 text-danger" title={pairs.map((p) => `${p.earlier} → ${p.later} at k=${fixed(p.k, 4)}`).join("; ")}>Calendar violation</span>}
                    {fit?.butterfly_ok && !pairs.length && <span className="text-muted">Grid checks pass</span>}
                    {!fit && <span className="text-warn">{e.svi_status === "too_few_points" ? "Too few points" : e.svi_status === "failed" ? "Fit failed" : "Not fitted"}</span>}
                  </div>
                </td>
                <td className="py-2 pr-2">
                  {fit ? <div className="flex flex-wrap gap-x-2 gap-y-0.5 tabular">
                    {(["a", "b", "rho", "m", "sigma"] as const).map((key) => <span key={key}>{key === "rho" ? "ρ" : key === "sigma" ? "σ" : key} {fixed(fit[key], 5)}</span>)}
                  </div> : <span className="break-words text-muted">{e.svi_reason ?? "No calibration available."}</span>}
                </td>
                <td className="py-2 text-right tabular">
                  <div>{fixed(fit?.rmse_vol_points, 4)}</div>
                  <div className="text-muted">{fit?.points ?? e.svi_points ?? "—"} pts</div>
                </td>
              </tr>
            )
          })}
        </tbody>
      </table>
    </div>
  )
}
