import type { AccountDamage, JournalSize } from "../api/trading-types"

export function AccountStorageBanner({ damaged, journal, reason }: { damaged?: AccountDamage | null; journal?: JournalSize; reason?: string | null }) {
  if (!damaged && !journal?.warning && !reason?.startsWith("JOURNAL_IO:")) return null
  return <div role="alert" className="border-b border-warn px-4 py-3 text-sm text-warn">
    {damaged && <><strong>Damaged account · read only</strong><p>{damaged.reason}</p>
      <p>Last verified record {damaged.last_good_seq}{damaged.last_good_time ? ` · ${damaged.last_good_time}` : " · no verified market time"}</p></>}
    {!damaged && reason?.startsWith("JOURNAL_IO:") && <p>{reason}</p>}
    {journal?.warning && <p>{journal.bytes.toLocaleString()} journal bytes · {journal.records.toLocaleString()} records. {journal.warning}</p>}
  </div>
}
