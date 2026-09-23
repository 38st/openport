import { useMemo, useState } from "react"
import type { Marker } from "./LineChart"
import { extent, linear, niceTicks, pad } from "./scale"
import { useSize } from "./useSize"

interface Props {
  bars: { x: number; value: number }[]
  markers?: Marker[]
  height?: number
  formatX: (x: number) => string
  formatY: (y: number) => string
}

const margin = { top: 14, right: 16, bottom: 28, left: 60 }

/// Signed bars along a numeric axis (exposure by strike): positive teal, negative violet.
export function BarChart({ bars, markers = [], height = 280, formatX, formatY }: Props) {
  const [ref, { width }] = useSize<HTMLDivElement>()
  const [hover, setHover] = useState<number | null>(null)

  const layout = useMemo(() => {
    const xDomain = extent(bars.map((b) => b.x))
    const yDomain = extent([0, ...bars.map((b) => b.value)])
    if (!xDomain || !yDomain || width === 0) return null
    const step = bars.length > 1 ? (xDomain[1] - xDomain[0]) / (bars.length - 1) : 1
    const x = linear([xDomain[0] - step / 2, xDomain[1] + step / 2], [margin.left, width - margin.right])
    const y = linear(pad(yDomain, 0.06), [height - margin.bottom, margin.top])
    const barWidth = Math.max(1, Math.abs(x(step) - x(0)) * 0.8)
    return { x, y, barWidth, xTicks: niceTicks(...xDomain, Math.max(2, Math.floor(width / 110))), yTicks: niceTicks(...y.domain, 5) }
  }, [bars, width, height])

  const hovered = hover == null ? null : bars[hover]

  return (
    <div ref={ref} className="relative w-full select-none" style={{ height }}>
      {layout && (
        <svg width={width} height={height} className="block" onPointerLeave={() => setHover(null)}>
          {layout.yTicks.map((t) => (
            <g key={t}>
              <line x1={margin.left} x2={width - margin.right} y1={layout.y(t)} y2={layout.y(t)} className={t === 0 ? "stroke-muted" : "stroke-border"} strokeDasharray={t === 0 ? undefined : "2 3"} />
              <text x={margin.left - 8} y={layout.y(t)} dy="0.32em" textAnchor="end" className="fill-muted tabular text-[10px]">
                {formatY(t)}
              </text>
            </g>
          ))}
          {layout.xTicks.map((t) => (
            <text key={t} x={layout.x(t)} y={height - 8} textAnchor="middle" className="fill-muted tabular text-[10px]">
              {formatX(t)}
            </text>
          ))}
          {bars.map((b, i) => {
            const y0 = layout.y(0)
            const y1 = layout.y(b.value)
            return (
              <rect
                key={b.x}
                x={layout.x(b.x) - layout.barWidth / 2}
                y={Math.min(y0, y1)}
                width={layout.barWidth}
                height={Math.max(0.5, Math.abs(y1 - y0))}
                className={b.value >= 0 ? "fill-gex-positive" : "fill-gex-negative"}
                opacity={hover == null || hover === i ? 0.9 : 0.45}
                onPointerEnter={() => setHover(i)}
              />
            )
          })}
          {markers.map((m) => (
            <g key={m.label}>
              <line x1={layout.x(m.x)} x2={layout.x(m.x)} y1={margin.top} y2={height - margin.bottom} style={{ stroke: m.color }} strokeDasharray="4 3" />
              <text x={layout.x(m.x) + 4} y={margin.top + 8} style={{ fill: m.color }} className="text-[10px]">
                {m.label}
              </text>
            </g>
          ))}
        </svg>
      )}
      {hovered && layout && (
        <div
          className="pointer-events-none absolute top-2 rounded-md border border-border bg-tooltip px-2 py-1 text-[11px] shadow-chart"
          style={{ left: Math.min(layout.x(hovered.x) + 10, width - 150) }}
        >
          <span className="text-muted tabular">{formatX(hovered.x)}</span>{" "}
          <span className="tabular">{formatY(hovered.value)}</span>
        </div>
      )}
    </div>
  )
}
