#!/usr/bin/env python3
"""Soak test: hold an SPX iron condor with far wings through a demo-market day and
check, every few seconds, that the account can still trade.

    tools/demo_soak.py URL [SPEED] [PLAN]

URL is an openportd to test, such as one started for it with
`openportd --port 8094 --no-history --paper-journal /tmp/soak/paper.jsonl`. The soak
replaces any replay running there and trades only the replay's own in-memory account.
SPEED is the replay's speed (default 120) and PLAN its plan (default practice).

Each probe places a non-marketable limit buy of the short put, a leg that keeps a
two-sided quote, and cancels it, so it asks whether the account can trade at all.
The report counts refused probes by code and portfolio snapshots with incomplete marks;
a healthy build refuses only by the account's own risk rules.
"""
import json
import sys
import time
import urllib.error
import urllib.request

BASE = sys.argv[1]
SPEED = int(sys.argv[2]) if len(sys.argv) > 2 else 120
PLAN = sys.argv[3] if len(sys.argv) > 3 else "practice"


def call(method, path, body=None):
    data = None if body is None else json.dumps(body).encode()
    request = urllib.request.Request(BASE + path, data=data, method=method,
                                     headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            return response.status, json.loads(response.read() or b"null")
    except urllib.error.HTTPError as error:
        return error.code, json.loads(error.read() or b"null")


def code(body):
    return (body or {}).get("error", {}).get("code") if isinstance(body, dict) else None


# A fresh replay of the default demo day under the End-of-day 50K plan.
call("DELETE", "/api/replay")
status, body = call("POST", "/api/replay", {"demo": "reversal", "speed": 30, "plan": PLAN})
assert status in (200, 201), (status, body)
while True:
    _, st = call("GET", "/api/replay/status")
    spx = next((u for u in st["underlyings"] if u["symbol"] == "SPX"), None)
    if spx and spx["options"] > 0 and spx["paper"]["accepting"]:
        break
    time.sleep(0.5)

# The condor: the second expiry, shorts near 10 delta, wings near 3 delta.
_, summary = call("GET", "/api/replay/underlyings/SPX/summary")
expiries = [e["id"] for e in summary["expiries"]]
expiry = expiries[1] if len(expiries) > 1 else expiries[0]
_, chain = call("GET", f"/api/replay/underlyings/SPX/chain?expiry={expiry}")
rows = chain["strikes"]


def pick(kind, target):
    best = None
    for row in rows:
        leg = row.get(kind)
        if not leg or leg.get("delta") is None or not leg.get("ask"):
            continue
        distance = abs(abs(leg["delta"]) - target)
        if best is None or distance < best[0]:
            best = (distance, leg)
    return best[1]


legs = {"short put": pick("put", 0.10), "long put": pick("put", 0.03),
        "short call": pick("call", 0.10), "long call": pick("call", 0.03)}
print("expiry", expiry)
for name, leg in legs.items():
    print(f"  {name:10s} {leg['symbol']}  bid {leg['bid']} ask {leg['ask']}  delta {leg['delta']:.3f}")
for name, side in (("long put", "buy"), ("long call", "buy"), ("short put", "sell"), ("short call", "sell")):
    status, body = call("POST", "/api/replay/orders", {
        "client_order_id": f"condor-{name.replace(' ', '-')}", "symbol": legs[name]["symbol"], "side": side,
        "type": "market", "quantity": 1, "time_in_force": "ioc"})
    print(f"  {side} {name}: {status} {body.get('order', {}).get('status') if status < 300 else body}")

# Play the rest of the day, probing as it goes.
call("PUT", "/api/replay", {"speed": SPEED})
probe = legs["short put"]["symbol"]
probes, refused, incomplete, samples = 0, {}, 0, 0
worst_age = 0.0
last_time = None
while True:
    _, replay = call("GET", "/api/replay")
    state = replay["replay"]
    _, portfolio = call("GET", "/api/replay/portfolio")
    samples += 1
    if not portfolio.get("valuation_complete", True):
        incomplete += 1
        if incomplete <= 3 or incomplete % 40 == 0:
            stale = [(pos["symbol"].split()[-1], pos.get("fresh"), pos.get("mark_age_seconds"), pos.get("awaiting_settlement"))
                     for pos in portfolio.get("positions", []) if not pos.get("fresh")]
            print("  incomplete at", portfolio.get("time"), "flags", portfolio.get("quality_flags"), "stale", stale,
                  "stocks", [(st.get("symbol"), st.get("fresh")) for st in portfolio.get("stocks", [])])
    for position in portfolio.get("positions", []):
        worst_age = max(worst_age, position.get("mark_age_seconds") or 0)
    _, chain = call("GET", f"/api/replay/underlyings/SPX/chain?expiry={expiry}")
    bid = next((row["put"]["bid"] for row in chain.get("strikes", [])
                if row.get("put", {}).get("symbol") == probe), None)
    if bid and bid > 0:
        probes += 1
        status, body = call("POST", "/api/replay/orders", {
            "client_order_id": f"probe-{probes}", "symbol": probe, "side": "buy", "type": "limit",
            "quantity": 1, "limit_price": f"{bid:.2f}", "time_in_force": "day"})
        if status == 201:
            order = body["order"]
            if order["status"] in ("working", "partially_filled"):
                call("DELETE", f"/api/replay/orders/{order['id']}")
        else:
            refused[code(body)] = refused.get(code(body), 0) + 1
    last_time = portfolio.get("time")
    if state.get("finished"):
        break
    time.sleep(1.0)

_, portfolio = call("GET", "/api/replay/portfolio")
print("ended at", last_time)
print("positions held:", len(portfolio.get("positions", [])), "| equity", portfolio.get("equity"))
print(f"probes {probes}, refused {sum(refused.values())} {refused}")
print(f"portfolio samples {samples}, with incomplete marks {incomplete}, oldest mark {worst_age:.0f} s")
