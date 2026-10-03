import { useQuery } from "@tanstack/react-query"
import { api } from "../api/client"
import { TradingError } from "./TradingControls"

export function CopySettingsPicker({ value, onChange, disabled = false }: { value: string; onChange: (id: string) => void; disabled?: boolean }) {
  const accounts = useQuery({ queryKey: ["live-accounts"], queryFn: ({ signal }) => api.liveAccounts(signal), staleTime: 5_000 })
  return <div className="space-y-1">
    <label className="block text-sm">Copy limits and guardrails from
      <select aria-label="Copy limits and guardrails from" className="trade-input" value={value} onChange={(event) => onChange(event.target.value)} disabled={disabled}>
        <option value="">Server defaults</option>
        {(accounts.data?.accounts ?? []).map((account) => <option key={account.id} value={account.id}>
          {account.name} · {account.id}{account.archived ? " (archived)" : ""}
        </option>)}
      </select>
    </label>
    <p className="text-xs text-muted">Copies active settings only. Queued changes and trading state stay with the source account.</p>
    <TradingError error={accounts.error} />
  </div>
}
