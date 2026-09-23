import type { Surface, SurfaceExpiry, SsviFit } from "../api/types"
import { expiryLabel, fixed } from "../lib/format"
import { smileColor } from "../lib/svi"

export function SviTable({ expiries, violations, ssvi }: { ssvi?: SsviFit; expiries: SurfaceExpiry[]; violations: NonNullable<Surface["calendar_violations"]> }) {
  if (!expiries.length) return null
  return (
    <div className="mt-4 border-t border-border pt-3">
      <h3 className="mb-2 text-xs font-medium text-muted">SVI / SSVI calibration · RMSE in vol points</h3>
      <p className="mb-2 text-[11px] text-muted">SSVI: one surface, arbitrage-free by construction; per-expiry SVI fits tighter but can admit arbitrage.</p>
      {ssvi && <div className="mb-3 flex flex-wrap gap-x-3 gap-y-1 text-[11px] tabular" aria-label="SSVI surface parameters">
        {ssvi.status === "ok" ? <>
          <span>ρ {fixed(ssvi.rho, 5)}</span><span>η {fixed(ssvi.eta, 5)}</span><span>γ {fixed(ssvi.gamma, 5)}</span>
          <span>Overall RMSE {fixed(ssvi.rmse_vol_points, 4)} vp</span><span>Fit {fixed(ssvi.fit_ms, 2)} ms</span>
          {ssvi.monotone_adjusted && <span className="text-warn">ATM total variance adjusted for monotonicity</span>}
        </> : <span className="text-warn">SSVI {ssvi.status === "too_few_points" ? "too few points" : "fit failed"}: {ssvi.reason}</span>}
      </div>}
      <table className="w-full table-fixed text-[11px]" aria-label="SVI and SSVI parameters and arbitrage diagnostics">
        <thead className="text-muted">
          <tr>
            <th className="w-[25%] pb-1 text-left font-normal">Expiry / checks</th>
            <th className="w-[40%] pb-1 text-left font-normal">a · b · ρ · m · σ</th>
            <th className="pb-1 text-right font-normal">SVI RMSE / points</th>
            <th className="pb-1 pl-2 text-right font-normal">SSVI RMSE / θ</th>
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
                    {pairs.map((p) => <span key={`${p.earlier}-${p.later}`} className="rounded border border-danger px-1 text-danger" title={`Later expiry ${expiryLabel(p.later, true)} needs ${fixed(p.vol_points, 2)} vol points more IV at k=${fixed(p.k, 4)}; tolerance ${fixed(p.tolerance_vol_points, 2)} vp`}>
                      calendar: {p.vol_points == null ? "violation" : `${fixed(p.vol_points, 1)} vp`} at k={fixed(p.k, 2)} vs {expiryLabel(p.earlier === e.id ? p.later : p.earlier)}
                    </span>)}
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
                <td className="py-2 pl-2 text-right tabular">
                  <div>{fixed(e.ssvi_rmse_vol_points, 4)}</div>
                  <div className="text-muted">θ {fixed(e.ssvi_theta, 5)}</div>
                  {e.ssvi_reason && <div className="break-words text-warn">{e.ssvi_reason}</div>}
                </td>
              </tr>
            )
          })}
        </tbody>
      </table>
    </div>
  )
}
