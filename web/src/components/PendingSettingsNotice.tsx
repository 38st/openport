export function PendingSettingsNotice({ requiresReset }: { requiresReset?: boolean }) {
  return requiresReset ? <p role="status" className="text-xs text-warn">
    Queued settings take effect at the next trading day, which this replay does not reach; reset the replay account to apply them now.
  </p> : null
}
