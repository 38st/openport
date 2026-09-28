import { useEffect } from "react"
import { useLive } from "../api/live"
import { isNewerVersion, updates, useUpdates } from "../lib/updates"
import { Panel } from "./ui"

export function UpdateSettings() {
  const { enabled } = useUpdates()
  const { status } = useLive()
  return (
    <Panel title="Updates">
      <div className="mb-3 text-xs text-muted">Running version: {status?.version ?? "unavailable"}</div>
      <label className="flex items-center gap-2 text-sm">
        <input type="checkbox" checked={enabled} onChange={(event) => updates.setEnabled(event.target.checked)} />
        Check for updates
      </label>
      <p className="mt-2 text-xs text-muted">
        Off by default. When on, this browser asks GitHub for the latest release at most once a day.
        No token, account data or running version is sent. Network failures stay silent.
      </p>
    </Panel>
  )
}

export function UpdateNotice() {
  const { status } = useLive()
  const { enabled, release, dismissed } = useUpdates()
  const current = status?.version
  useEffect(() => {
    if (!enabled) return
    const check = () => { void updates.check(current) }
    check()
    const timer = setInterval(check, 60_000)
    return () => clearInterval(timer)
  }, [enabled, current])
  if (!enabled || !release || release.version === dismissed || !current || !isNewerVersion(release.version, current)) return null
  return (
    <div role="status" className="flex flex-wrap items-center gap-x-3 gap-y-1 border-b border-border bg-panel px-4 py-2 text-xs text-muted">
      <span>OpenPort {release.version} is available.</span>
      <a href={release.url} target="_blank" rel="noopener noreferrer" className="text-accent underline decoration-dotted">View release</a>
      <button type="button" onClick={() => updates.dismiss(release.version)} aria-label="Dismiss update notice"
        className="ml-auto rounded px-2 py-1 hover:bg-raised hover:text-foreground">Dismiss</button>
    </div>
  )
}
