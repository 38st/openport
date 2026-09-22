import { useState, type ReactNode } from "react"
import { flashDirection, type FlashDirection } from "../lib/flash"

// Ported from Gambit. When an existing value changes, the wrapper flashes green or
// red once and settles. The first render never flashes (no baseline). The span
// remounts per revision so the animation restarts; deriving state from a changed
// prop during render keeps the previous value out of refs.
function useFlash(value: number | null | undefined) {
  const [state, setState] = useState(() => ({ value, direction: null as FlashDirection, revision: 0 }))
  if (!Object.is(value, state.value)) {
    setState({ value, direction: flashDirection(state.value, value), revision: state.revision + 1 })
  }
  return state
}

export function Flash({ value, children, className = "" }: { value: number | null | undefined; children: ReactNode; className?: string }) {
  const { direction, revision } = useFlash(value)
  const animation = direction === "up" ? "animate-flash-up" : direction === "down" ? "animate-flash-down" : ""
  return (
    <span key={revision} className={`rounded-sm ${animation} ${className}`}>
      {children}
    </span>
  )
}
