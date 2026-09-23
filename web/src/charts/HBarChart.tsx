export interface HBar {
  label: string
  value: number | null
}

/// Horizontal bars in a readable list. Signed values grow either side of a
/// centre zero line (gains bullish, losses bearish); unsigned values grow from
/// the left in the accent colour. Empty rows keep their label and track.
export function HBarChart({ rows, format, signed = false, label, domain }: {
  rows: HBar[]
  format: (value: number) => string
  signed?: boolean
  label: string
  /** Fixed maximum magnitude, e.g. 1 for rates; otherwise the largest value. */
  domain?: number
}) {
  const max = (domain ?? Math.max(0, ...rows.map((r) => (r.value == null ? 0 : Math.abs(r.value))))) || 1
  const anyNegative = signed && rows.some((r) => (r.value ?? 0) < 0)
  return (
    <ul aria-label={label} className="space-y-1.5">
      {rows.map((row) => {
        const value = row.value
        const width = value == null ? 0 : Math.min(1, Math.abs(value) / max) * (anyNegative ? 50 : 100)
        const negative = value != null && value < 0
        const left = anyNegative ? (negative ? 50 - width : 50) : 0
        return (
          <li key={row.label} className="grid grid-cols-[4.5rem_1fr_5.5rem] items-center gap-2 text-[11px]"
            aria-label={`${row.label}: ${value == null ? "no trades" : format(value)}`}>
            <span className="truncate text-right text-muted">{row.label}</span>
            <span className="relative h-3 rounded-sm bg-raised">
              {anyNegative && <span className="absolute inset-y-0 left-1/2 w-px bg-border" />}
              {width > 0 && <span className={`absolute inset-y-0 rounded-sm ${signed ? (negative ? "bg-bearish" : "bg-bullish") : "bg-accent"}`}
                style={{ left: `${left}%`, width: `${Math.max(width, 0.6)}%` }} />}
            </span>
            <span className={`tabular text-right ${value == null ? "text-faint" : signed ? (negative ? "text-bearish" : value > 0 ? "text-bullish" : "") : ""}`}>
              {value == null ? "—" : format(value)}
            </span>
          </li>
        )
      })}
    </ul>
  )
}
