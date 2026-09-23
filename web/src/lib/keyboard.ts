/** Roving focus for radio groups and expiry tabs. */
export function groupKeyIndex(key: string, index: number, count: number): number | null {
  if (count <= 0) return null
  if (key === "Home") return 0
  if (key === "End") return count - 1
  if (key === "ArrowRight" || key === "ArrowDown") return (index + 1) % count
  if (key === "ArrowLeft" || key === "ArrowUp") return (index - 1 + count) % count
  return null
}
