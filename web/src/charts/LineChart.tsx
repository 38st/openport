import { useMemo, useState, type PointerEvent } from "react"
import { extent, linear, niceTicks, pad } from "./scale"
import { useSize } from "./useSize"

export interface SeriesPoint {
  x: number
  y: number | null
  lo?: number | null
  hi?: number | null
}

export interface Series {
  id: string
  label: string
  color: string
  points: SeriesPoint[]
  band?: boolean
}

export interface Marker {
  x: number
  label: string
  color: string
}

interface Props {
  series: Series[]
  markers?: Marker[]
  height?: number
  formatX: (x: number) => string
  formatY: (y: number) => string
  xLabel?: string
  /** Tick positions for a transformed axis; round numbers are chosen otherwise. */
  xTicks?: number[]
}

const margin = { top: 12, right: 16, bottom: 28, left: 52 }

/// Multi-series line chart with optional bid/ask bands, vertical markers and a
/// crosshair that reads out every series at the pointer.
export function LineChart({ series, markers = [], height = 320, formatX, formatY, xLabel, xTicks }: Props) {
  const [ref, { width }] = useSize<HTMLDivElement>()
  const [hoverX, setHoverX] = useState<number | null>(null)

  const layout = useMemo(() => {
    const xs = series.flatMap((s) => s.points.map((p) => p.x))
    const ys = series.flatMap((s) =>
      s.points.flatMap((p) => (s.band ? [p.y, p.lo, p.hi] : [p.y])),
    )
    const xDomain = extent(xs)
    const yDomain = extent(ys)
    if (!xDomain || !yDomain || width === 0) return null
    const x = linear(xDomain, [margin.left, width - margin.right])
    const y = linear(pad(yDomain, 0.08), [height - margin.bottom, margin.top])
    const ticks = xTicks
      ? xTicks.filter((t) => t >= xDomain[0] && t <= xDomain[1])
      : niceTicks(...xDomain, Math.max(2, Math.floor(width / 110)))
    return { x, y, xTicks: ticks, yTicks: niceTicks(...y.domain, 5) }
  }, [series, width, height, xTicks])

  const path = (points: SeriesPoint[], pick: (p: SeriesPoint) => number | null | undefined) => {
    if (!layout) return ""
    let d = ""
    let pen = false
    for (const p of points) {
      const v = pick(p)
      if (v == null || !Number.isFinite(v)) {
        pen = false
        continue
      }
      d += `${pen ? "L" : "M"}${layout.x(p.x).toFixed(1)},${layout.y(v).toFixed(1)}`
      pen = true
    }
    return d
  }

  const band = (points: SeriesPoint[]) => {
    if (!layout) return ""
    const valid = points.filter((p) => p.lo != null && p.hi != null && Number.isFinite(p.lo) && Number.isFinite(p.hi))
    if (valid.length < 2) return ""
    const top = valid.map((p, i) => `${i ? "L" : "M"}${layout.x(p.x).toFixed(1)},${layout.y(p.hi as number).toFixed(1)}`)
    const bottom = [...valid].reverse().map((p) => `L${layout.x(p.x).toFixed(1)},${layout.y(p.lo as number).toFixed(1)}`)
    return `${top.join("")}${bottom.join("")}Z`
  }

  const onMove = (event: PointerEvent<SVGSVGElement>) => {
    if (!layout) return
    const rect = event.currentTarget.getBoundingClientRect()
    setHoverX(layout.x.invert(event.clientX - rect.left))
  }

  const readout =
    hoverX == null
      ? []
      : series.map((s) => {
          let best: SeriesPoint | undefined
          for (const p of s.points) {
            if (p.y == null) continue
            if (!best || Math.abs(p.x - hoverX) < Math.abs(best.x - hoverX)) best = p
          }
          return { series: s, point: best }
        })
  const anchor = readout.find((r) => r.point)?.point

  return (
    <div ref={ref} className="relative w-full select-none" style={{ height }}>
      {layout && (
        <svg width={width} height={height} onPointerMove={onMove} onPointerLeave={() => setHoverX(null)} className="block">
          {layout.yTicks.map((t) => (
            <g key={`y${t}`}>
              <line x1={margin.left} x2={width - margin.right} y1={layout.y(t)} y2={layout.y(t)} className="stroke-border" strokeDasharray="2 3" />
              <text x={margin.left - 8} y={layout.y(t)} dy="0.32em" textAnchor="end" className="fill-muted tabular text-[10px]">
                {formatY(t)}
              </text>
            </g>
          ))}
          {layout.xTicks.map((t) => (
            <text key={`x${t}`} x={layout.x(t)} y={height - 8} textAnchor="middle" className="fill-muted tabular text-[10px]">
              {formatX(t)}
            </text>
          ))}
          {xLabel && (
            <text x={width - margin.right} y={height - 8} textAnchor="end" className="fill-faint text-[10px]">
              {xLabel}
            </text>
          )}
          {markers.map((m) => (
            <g key={m.label}>
              <line x1={layout.x(m.x)} x2={layout.x(m.x)} y1={margin.top} y2={height - margin.bottom} stroke={m.color} strokeDasharray="4 3" strokeOpacity={0.8} />
              <text x={layout.x(m.x) + 4} y={margin.top + 10} fill={m.color} className="text-[10px]">
                {m.label}
              </text>
            </g>
          ))}
          {series.map((s) =>
            s.band ? <path key={`band-${s.id}`} d={band(s.points)} fill={s.color} fillOpacity={0.12} /> : null,
          )}
          {series.map((s) => (
            <path key={s.id} d={path(s.points, (p) => p.y)} fill="none" stroke={s.color} strokeWidth={1.6} strokeLinejoin="round" />
          ))}
          {anchor && hoverX != null && (
            <line x1={layout.x(anchor.x)} x2={layout.x(anchor.x)} y1={margin.top} y2={height - margin.bottom} className="stroke-muted" strokeOpacity={0.5} />
          )}
          {readout.map(({ series: s, point }) =>
            point && point.y != null ? (
              <circle key={`dot-${s.id}`} cx={layout.x(point.x)} cy={layout.y(point.y)} r={3} fill={s.color} className="stroke-background" strokeWidth={1.5} />
            ) : null,
          )}
        </svg>
      )}
      {anchor && hoverX != null && layout && (
        <div
          className="pointer-events-none absolute top-2 rounded-md border border-border bg-raised/95 px-2 py-1.5 text-[11px] shadow-lg"
          style={{ left: Math.min(layout.x(anchor.x) + 12, width - 170) }}
        >
          <div className="mb-1 text-muted tabular">{formatX(anchor.x)}</div>
          {readout.map(({ series: s, point }) =>
            point && point.y != null ? (
              <div key={s.id} className="flex items-center justify-between gap-3">
                <span className="flex items-center gap-1.5">
                  <span className="inline-block h-1.5 w-1.5 rounded-full" style={{ background: s.color }} />
                  {s.label}
                </span>
                <span className="tabular">{formatY(point.y)}</span>
              </div>
            ) : null,
          )}
        </div>
      )}
    </div>
  )
}
