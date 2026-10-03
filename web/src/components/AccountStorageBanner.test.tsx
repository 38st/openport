import { renderToStaticMarkup } from "react-dom/server"
import { expect, it } from "vitest"
import { AccountStorageBanner } from "./AccountStorageBanner"

it("shows damaged account recovery instructions and the verified boundary", () => {
  const html = renderToStaticMarkup(<AccountStorageBanner damaged={{ reason: "Torn line; stop and run --repair-journals", last_good_seq: 12, last_good_time: "2026-09-22T14:00:00Z" }} />)
  expect(html).toContain("Damaged account · read only")
  expect(html).toContain("--repair-journals")
  expect(html).toContain("Last verified record 12")
  expect(html).toContain("2026-09-22T14:00:00Z")
})
it("shows named disk failures and journal warnings", () => {
  const html = renderToStaticMarkup(<AccountStorageBanner reason="JOURNAL_IO: filesystem device 42 has 12345 free bytes" journal={{ bytes: 268435456, records: 123, warning: "Stop and run --compact-journals" }} />)
  expect(html).toContain("12345 free bytes")
  expect(html).toContain("123 records")
  expect(html).toContain("--compact-journals")
})
