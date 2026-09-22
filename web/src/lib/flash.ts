// From Gambit: which way a live value moved, if it moved at all.
export type FlashDirection = "up" | "down" | null

export function flashDirection(previous: number | null | undefined, value: number | null | undefined): FlashDirection {
  if (previous == null || value == null || !Number.isFinite(previous) || !Number.isFinite(value) || previous === value) {
    return null
  }
  return value > previous ? "up" : "down"
}
