import type { Coverage, Num } from "../api/types"
import { fixed, isNum, pct } from "./format"

export function expiryCoverage(coverage: Coverage | null | undefined) {
  if (!coverage) return null
  const { options, priced, open_interest: oi } = coverage
  const low = isNum(options) && options > 0
    && ((isNum(priced) && priced / options < 0.9) || (isNum(oi) && oi / options < 0.9))
  return { label: `priced ${fixed(priced, 0)}/${fixed(options, 0)} · OI ${fixed(oi, 0)}/${fixed(options, 0)}`, low }
}

export function oiCoverage(ratio: Num | undefined) {
  return { label: `OI coverage ${pct(ratio, 0)}`, low: isNum(ratio) && ratio < 0.9 }
}
