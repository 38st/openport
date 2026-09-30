"""Offline tests for demo_soak.py against a scripted replay API; no server runs."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import demo_soak  # noqa: E402

SHORT_PUT = "SPXW  260918P05900000"


def leg(symbol, delta, bid=1.00, ask=1.20):
    return {"symbol": symbol, "delta": delta, "bid": bid, "ask": ask}


class FakeReplay:
    """The replay routes the soak calls. The day ends after `snapshots` pauses; the
    probe's quote and what an order does can be scripted."""

    def __init__(self, snapshots=3, quote=(2.90, 3.10), fills=False, finish_before_probe=False):
        self.snapshots, self.quote, self.fills = snapshots, quote, fills
        self.finish_before_probe = finish_before_probe
        self.paused, self.polls, self.finished = False, 0, False
        self.started, self.orders, self.cancelled = None, [], []

    def chain(self):
        bid, ask = self.quote
        return {"strikes": [
            {"strike": 5900, "put": leg(SHORT_PUT, -0.10, bid, ask), "call": leg("SPXW  260918C06200000", 0.10)},
            {"strike": 5800, "put": leg("SPXW  260918P05800000", -0.03), "call": leg("SPXW  260918C06300000", 0.03)}]}

    def call(self, method, path, body=None):
        if path == "/api/replay" and method == "DELETE":
            return 200, {"replay": None}
        if path == "/api/replay" and method == "POST":
            self.started = body
            return 201, {"replay": {"scenario": body["demo"], "seed": body.get("seed", "123")}}
        if path == "/api/replay" and method == "PUT":
            if "paused" in body:
                self.paused = body["paused"]
                if self.paused:
                    self.polls += 1
                    self.finished = self.polls > self.snapshots
            return 200, {"replay": {}}
        if path == "/api/replay" and method == "GET":
            return 200, {"replay": {"finished": self.finished}}
        if path == "/api/replay/status":
            return 200, {"underlyings": [{"symbol": "SPX", "options": 10, "paper": {"accepting": True}}]}
        if path == "/api/replay/underlyings/SPX/summary":
            return 200, {"expiries": [{"id": "2026-09-17PM"}, {"id": "2026-09-18PM"}]}
        if path.startswith("/api/replay/underlyings/SPX/chain"):
            return 200, self.chain()
        if path == "/api/replay/risk":
            return 200, {"limits": {"price_band_absolute": "0.50", "price_band_relative": 0.2}}
        if path == "/api/replay/portfolio":
            held = {order["symbol"]: 0 for order in self.orders}
            for order in self.orders:
                if order["filled_quantity"]:
                    held[order["symbol"]] += order["filled_quantity"] * (1 if order["side"] == "buy" else -1)
            return 200, {"time": "t", "valuation_complete": True, "equity": "100000.00",
                         "positions": [{"symbol": s, "quantity": q, "fresh": True} for s, q in held.items() if q]}
        if path == "/api/replay/orders" and method == "POST":
            if self.finish_before_probe and body["client_order_id"].startswith("probe-"):
                return 403, {"error": {"code": "REPLAY_READ_ONLY", "message": "The replay has finished"}}
            order = dict(body, id=str(len(self.orders) + 1), paused=self.paused, filled_quantity=0, status="working")
            if body["type"] == "market" or (self.fills and body["client_order_id"].startswith("probe-")):
                order.update(status="filled", filled_quantity=body["quantity"])
            self.orders.append(order)
            return 201, {"order": order}
        if path.startswith("/api/replay/orders/") and method == "DELETE":
            order = self.orders[int(path.rsplit("/", 1)[1]) - 1]
            order["status"] = "cancelled"
            self.cancelled.append(order["id"])
            return 200, {"order": order}
        raise AssertionError(f"unexpected {method} {path}")


def run(replay, seed=None):
    lines = []
    result = demo_soak.soak(replay, speed=300, seed=seed, out=lambda *parts: lines.append(" ".join(map(str, parts))), pause=0)
    return result, "\n".join(lines)


class DemoSoakTest(unittest.TestCase):
    def test_probe_price_is_under_the_ask_and_inside_the_band(self):
        for bid, ask, expected in ((2.90, 3.10, 2.90), (3.00, 3.10, 3.00), (3.05, 3.20, 3.00),
                                   (0.10, 0.20, 0.10), (1.00, 3.00, 1.50), (5.00, 5.00, None), (0, 0.05, None)):
            price = demo_soak.probe_price(bid, ask)
            self.assertEqual(price, expected, (bid, ask))
            if price is not None:
                mid = (bid + ask) / 2
                self.assertLess(price, ask)
                self.assertLessEqual(mid - price, max(0.5, 0.2 * mid) + 1e-9)

    def test_probes_go_out_paused_and_leave_the_condor_whole(self):
        replay = FakeReplay()
        result, text = run(replay, seed=81723)
        self.assertEqual(replay.started["seed"], "81723")
        probes = [order for order in replay.orders if order["client_order_id"].startswith("probe-")]
        self.assertEqual(len(probes), 3)
        self.assertTrue(all(order["paused"] and order["limit_price"] == "2.90" for order in probes))
        self.assertEqual(replay.cancelled, [order["id"] for order in probes])
        self.assertEqual((result["probe_fills"], result["refused"]), (0, {}))
        self.assertTrue(result["condor_intact"])
        self.assertIn("seed 81723", text)

    def test_a_filled_probe_is_reported_and_sold_back(self):
        replay = FakeReplay(snapshots=1, fills=True)
        result, text = run(replay)
        self.assertEqual((result["probe_fills"], result["restored"]), (1, 1))
        self.assertTrue(result["condor_intact"])
        self.assertIn("filled 1 (sold back 1)", text)

    def test_a_probe_after_the_day_ends_is_not_a_refusal(self):
        result, _ = run(FakeReplay(finish_before_probe=True))
        self.assertEqual((result["probes"], result["refused"]), (0, {}))
        replay = FakeReplay(snapshots=0)
        result, _ = run(replay)
        self.assertFalse(any(order["client_order_id"].startswith("probe-") for order in replay.orders))


if __name__ == "__main__":
    unittest.main()
