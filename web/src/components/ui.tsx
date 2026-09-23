import type { ReactNode } from "react"
import type { FeedState } from "../api/types"
import { groupKeyIndex } from "../lib/keyboard"

export function Panel({ title, actions, children, className = "" }: { title?: ReactNode; actions?: ReactNode; children: ReactNode; className?: string }) {
  return (
    <section className={`rounded-lg border border-border bg-panel ${className}`}>
      {(title || actions) && (
        <header className="flex flex-wrap items-center justify-between gap-2 border-b border-border px-3 py-2">
          <h2 className="text-xs font-medium uppercase tracking-wide text-muted">{title}</h2>
          {actions && <div className="flex flex-wrap items-center gap-2">{actions}</div>}
        </header>
      )}
      <div className="p-3">{children}</div>
    </section>
  )
}

export function Stat({ label, value, hint, tone }: { label: string; value: ReactNode; hint?: string; tone?: "positive" | "negative" | "warn" }) {
  const colour = tone === "positive" ? "text-gex-positive" : tone === "negative" ? "text-gex-negative" : tone === "warn" ? "text-warn" : ""
  return (
    <div className="rounded-lg border border-border bg-panel px-3 py-2" title={hint}>
      <div className="text-[11px] uppercase tracking-wide text-muted">{label}</div>
      <div className={`mt-0.5 text-lg tabular ${colour}`}>{value}</div>
    </div>
  )
}

/// A row of mutually exclusive options.
export function Segmented<T extends string | number>({
  value,
  options,
  onChange,
  label,
}: {
  value: T
  options: { value: T; label: string }[]
  onChange: (value: T) => void
  label: string
}) {
  return (
    <div role="radiogroup" aria-label={label} className="inline-flex rounded-md border border-border bg-background p-0.5">
      {options.map((o, index) => (
        <button
          key={String(o.value)}
          type="button"
          role="radio"
          aria-checked={o.value === value}
          tabIndex={o.value === value ? 0 : -1}
          onClick={() => onChange(o.value)}
          onKeyDown={(event) => {
            if (event.altKey || event.ctrlKey || event.metaKey) return
            const next = groupKeyIndex(event.key, index, options.length)
            if (next == null) return
            const option = options[next]
            if (!option) return
            event.preventDefault()
            event.stopPropagation()
            onChange(option.value)
            event.currentTarget.parentElement?.querySelectorAll<HTMLButtonElement>('[role="radio"]').item(next)?.focus()
          }}
          className={`rounded px-2 py-0.5 text-xs transition-colors ${
            o.value === value ? "bg-raised text-foreground" : "text-muted hover:text-foreground"
          }`}
        >
          {o.label}
        </button>
      ))}
    </div>
  )
}

const feedTone: Record<FeedState, string> = {
  live: "bg-live",
  delayed: "bg-stale",
  connecting: "bg-warn",
  stale: "bg-warn",
  error: "bg-danger",
  stopped: "bg-faint",
}

export function FeedBadge({ state, message }: { state: FeedState; message: string }) {
  return (
    <span className="inline-flex items-center gap-1.5 rounded-full border border-border px-2 py-0.5 text-[11px]" title={message}>
      <span className={`h-1.5 w-1.5 rounded-full ${feedTone[state]} ${state === "live" ? "animate-pulse" : ""}`} />
      {state}
    </span>
  )
}

export function Empty({ children }: { children: ReactNode }) {
  return <div className="flex min-h-40 items-center justify-center text-sm text-muted">{children}</div>
}
