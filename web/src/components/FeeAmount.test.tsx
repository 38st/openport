import { renderToStaticMarkup } from "react-dom/server"
import { expect, it } from "vitest"
import { feeScheduleText } from "../lib/fees"
import { FeeAmount } from "./FeeAmount"

it("shows all fee parts and keeps old flat fills usable", () => {
  const html = renderToStaticMarkup(<FeeAmount fee="3.44" fees={{ commission: "2.00", clearing: "0.20", regulatory: "0.04", index: "1.20" }} />)
  for (const text of ["$3.44", "Fee breakdown", "Commission $2.00", "clearing $0.20", "regulatory $0.04", "index $1.20"]) expect(html).toContain(text)
  expect(renderToStaticMarkup(<FeeAmount fee="0.65" />)).toBe("$0.65")
  expect(renderToStaticMarkup(<FeeAmount fee={null} />)).toBe("—")
})

it("describes commission caps and exercise charges with their correct units", () => {
  const fees = { open: "1.00", close: "0.00", leg_cap: "10.00", clearing: "0.10", regulatory: "0.02", index: { SPXW: "0.60" }, exercise: "5.00" }
  const text = feeScheduleText(fees)
  expect(text).toContain("$10.00 per leg per order across partial fills")
  expect(text).toContain("$0.60 on SPXW")
  expect(text).toContain("$5.00 per contract; cash settlement is free")
  expect(feeScheduleText({ ...fees, leg_cap: "0.00" })).toContain("uncapped")
})
