/** Used for both placeholder data and fetched data: never label another instrument's payload as the selection. */
export function matchingPayload<T extends { symbol: string; expiry?: { id: string } }>(
  data: T | undefined,
  symbol: string,
  expiry?: string | null,
): T | undefined {
  return data?.symbol === symbol && (expiry === undefined || data.expiry?.id === expiry) ? data : undefined
}
