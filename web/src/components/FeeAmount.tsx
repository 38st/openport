import type { FillFees, Money } from "../api/trading-types"
import { feeItems } from "../lib/fees"
import { formatMoney } from "../lib/trading"

export function FeeAmount({ fee, fees }: { fee: Money | null | undefined; fees?: FillFees | null }) {
  return <>{formatMoney(fee)}{fees && <details className="text-xs text-muted">
    <summary className="cursor-pointer">Fee breakdown</summary>
    <p className="max-w-xs whitespace-normal">{feeItems(fees)}</p>
  </details>}</>
}
