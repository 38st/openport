import type { Fill } from "../api/trading-types"
import { fillBook } from "../lib/orders"

/** The bid × ask a fill traded against, with sizes, the paper size left and the quote's age; a dash before fills kept it. */
export function FillBook({ fill }: { fill: Fill }) {
  const book = fillBook(fill)
  if (!book || !fill.quote) return <span className="text-faint" title="Recorded before fills kept their quote">—</span>
  return <span title={`Observation ${fill.quote.observation}, first quoted ${fill.quote.quoted_at}`}>
    {book.book}<div className="text-[10px] text-faint">{book.detail}</div>
  </span>
}
