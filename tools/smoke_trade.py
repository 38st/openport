#!/usr/bin/env python3
"""Smoke test for writes: start the demo market and fill one order in it.

    tools/smoke_trade.py URL [TOKEN]

Starts the demo market's replay on the openportd at URL, buys one SPX call at the
market in the replay's own account, and stops the replay. The live accounts are not
touched. TOKEN is the server's write token, if it has one. Exits nonzero, saying why,
unless the order fills.
"""
import json
import sys
import time
import urllib.error
import urllib.request

BASE = sys.argv[1].rstrip("/")
TOKEN = sys.argv[2] if len(sys.argv) > 2 else ""


def call(method, path, body=None):
    headers = {"Content-Type": "application/json"}
    if TOKEN:
        headers["Authorization"] = f"Bearer {TOKEN}"
    data = None if body is None else json.dumps(body).encode()
    request = urllib.request.Request(BASE + path, data=data, method=method, headers=headers)
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            return response.status, json.loads(response.read() or b"null")
    except urllib.error.HTTPError as error:
        return error.code, json.loads(error.read() or b"null")


def fail(message):
    sys.exit(f"smoke trade: {message}")


status, body = call("POST", "/api/replay", {"demo": True})
if status != 201:
    fail(f"starting the demo market returned {status}: {body}")
deadline = time.time() + 60
while True:
    _, state = call("GET", "/api/replay/status")
    spx = next((u for u in (state or {}).get("underlyings", []) if u["symbol"] == "SPX"), None)
    if spx and spx["options"] > 0 and spx["paper"]["accepting"]:
        break
    if time.time() > deadline:
        fail(f"the demo market did not open for trading: {spx}")
    time.sleep(0.5)

_, chain = call("GET", "/api/replay/underlyings/SPX/chain")
calls = [row["call"] | {"strike": row["strike"]} for row in chain["strikes"]
         if (row.get("call") or {}).get("bid") and row["call"].get("ask")]
if not calls:
    fail("the demo chain has no two-sided call")
leg = min(calls, key=lambda c: abs(c["strike"] - chain["spot"]))
status, body = call("POST", "/api/replay/orders", {
    "client_order_id": "smoke", "symbol": leg["symbol"], "side": "buy", "type": "market",
    "quantity": 1, "time_in_force": "ioc"})
call("DELETE", "/api/replay")
if status != 201 or body["order"]["status"] != "filled":
    fail(f"the order was not filled: {status} {body}")
print(f"smoke trade: bought 1 {leg['symbol'].split()[-1]} at {body['fills'][0]['price']} in the demo market")
