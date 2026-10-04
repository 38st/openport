#!/usr/bin/env python3
"""Random orders and exact accounting checks on an isolated, paused replay.

    tools/order_fuzz.py URL --scenario hold-overnight --ops 300 --repeat

Replaces any existing replay. Never trades the live account. --seed selects the
market (default 81723); --fuzz-seed selects commands (default 1). JSON includes the
literal command tape and each command's market time, so failures can be reproduced.
All dependencies are Python's standard library.

Docs: Money is decimal dollars; position market_value already includes quantity,
multiplier and sign. /fills contains option executions; /trades contains signed
stock_fills (including deliveries). /settlements removes signed option quantities.
Early assignments also remove options; expiry assignments must not be counted twice.
Successful abandon commands remove the entire pre-command long at zero. Dividends
change cash, never quantities. No resets are generated, so ledgers span one attempt.

Buying power may be negative after market moves, with enforcement disabled, or
when a command frees power (docs/paper-trading.md, Margin and buying power). Check
accepted orders against previews and flag a deterioration of both free and available
power below zero, rather than treating every negative snapshot as a violation.

Scenario journals contain no run-specific fields to normalize (docs/scenarios.md
and docs/runtime.md). Compare raw head/count/bytes, and raw files when accessible.
History's `file` is the recording, NOT the account journal. Current servers expose
no journal path: supply --replays-dir, the replays/ beside --paper-journal, for CLI
verification or byte comparison. Verification never repairs or compacts a journal.
"""
import argparse
from collections import Counter
import copy
from datetime import datetime
from decimal import Decimal, InvalidOperation, ROUND_HALF_UP
import filecmp
import http.client
import json
from pathlib import Path
import random
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request


PREFIX = "/api/replay"
WORKING = {"working", "partially_filled", "armed"}


class FuzzError(Exception):
    """Transport, protocol, lifecycle or verification failure."""


def money(value):
    result = Decimal(str(value))
    if not result.is_finite():
        raise ValueError(f"non-finite amount: {value}")
    return result


def dumps(value):
    return json.dumps(value, sort_keys=True, indent=2, allow_nan=False, default=str) + "\n"


def error_code(body):
    error = body.get("error")
    return error.get("code") if isinstance(error, dict) else None


class Client:
    def __init__(self, base, token=None, timeout=3600):
        self.base, self.token, self.timeout = base.rstrip("/"), token, timeout

    def call(self, method, path, body=None):
        headers = {"Content-Type": "application/json"}
        if self.token:
            headers["Authorization"] = "Bearer " + self.token
        request = urllib.request.Request(
            self.base + path, method=method, headers=headers,
            data=None if body is None else json.dumps(body, allow_nan=False).encode())
        try:
            try:
                response = urllib.request.urlopen(request, timeout=self.timeout)
            except urllib.error.HTTPError as exc:
                response = exc
            with response:
                status, raw = response.code, response.read()
            result = json.loads(raw, parse_constant=lambda x: (_ for _ in ()).throw(ValueError(x)))
            if not isinstance(result, dict):
                raise ValueError("expected a JSON object")
            return status, result
        except (OSError, ValueError, http.client.HTTPException) as exc:
            # Do not echo headers or token; never retry an uncertain stock trade.
            raise FuzzError(f"{method} {path}: {type(exc).__name__}: {exc}") from exc


def request(client, method, path, body=None, refusals=None):
    status, result = client.call(method, path, body)
    if 200 <= status < 300:
        return status, result
    code = error_code(result)
    if 400 <= status < 500 and isinstance(code, str) and code and refusals is not None:
        refusals[code] += 1
        return status, result
    raise FuzzError(f"{method} {path}: HTTP {status}: {result}")


def get(client, path):
    return request(client, "GET", path)[1]


def snapshot(client):
    return {name: get(client, PREFIX + "/" + name) for name in
            ("account", "portfolio", "fills", "orders", "trades", "settlements")}


def signed(fill):
    return int(fill["quantity"]) * (1 if fill["side"] == "buy" else -1)


def check_invariants(state, before=None, accepted_order=False, abandoned=None):
    """Return structured failures and counters, with no tolerance or float arithmetic."""
    failures, counts = [], Counter(samples=1)

    def fail(check, **values):
        failures.append(dict(check=check, **values))

    portfolio, account = state["portfolio"], state["account"]
    positions, stocks = portfolio["positions"], portfolio["stocks"]
    marked = positions + stocks
    for holdings, quantity_field, multiplier in ((positions, "quantity", 100), (stocks, "shares", 1)):
        for position in holdings:
            if position["mark"] is None or position["market_value"] is None:
                continue
            expected = money(position["mark"]) * money(position[quantity_field]) * multiplier
            actual = money(position["market_value"])
            if actual != expected:
                fail("position_market_value", symbol=position["symbol"], actual=str(actual),
                     expected=str(expected), difference=str(actual - expected))
    if any(p["mark"] is None or p["market_value"] is None for p in marked):
        counts["equity_skipped_missing_marks"] += 1
    else:
        expected = money(portfolio["cash"]) + sum((money(p["market_value"]) for p in marked), Decimal(0))
        actual = money(portfolio["equity"])
        if actual != expected:
            fail("equity", actual=str(actual), expected=str(expected), difference=str(actual - expected),
                 cash=portfolio["cash"], market_values=[p["market_value"] for p in marked])
    if money(account["evaluation"]["equity"]) != money(portfolio["equity"]):
        fail("account_equity", account=account["evaluation"]["equity"], portfolio=portfolio["equity"])

    quantities, stock_quantities, fill_totals = Counter(), Counter(), Counter()
    option_fills = state["fills"]["fills"]
    for fill in option_fills:
        quantities[fill["symbol"]] += signed(fill)
        fill_totals[fill["order_id"], fill["symbol"]] += int(fill["quantity"])
    settlements = state["settlements"]["settlements"]
    settled = {(s["symbol"], s["time"]) for s in settlements}
    for settlement in settlements:
        quantities[settlement["symbol"]] -= int(settlement["quantity"])
    for fill in state["trades"]["stock_fills"]:
        stock_quantities[fill["symbol"]] += int(fill["shares"])
        option = fill.get("option")
        if option and fill["source"] in ("early_exercise", "assignment") and (option, fill["time"]) not in settled:
            # OSI suffix: YYMMDD C/P strike; every supported deliverable is 100 shares.
            contracts = Decimal(fill["shares"]) / 100 * (1 if option[-9] == "C" else -1)
            quantities[option] -= contracts
    for symbol, quantity in (abandoned or {}).items():
        quantities[symbol] -= quantity
    for name, ledger, held, field in (("option_quantity", quantities, positions, "quantity"),
                                      ("stock_quantity", stock_quantities, stocks, "shares")):
        actual = {p["symbol"]: int(p[field]) for p in held}
        for symbol in sorted(set(actual) | set(ledger)):
            if actual.get(symbol, 0) != ledger[symbol]:
                fail(name, symbol=symbol, actual=actual.get(symbol, 0), expected=ledger[symbol])
    filled_symbols = {f["symbol"] for f in option_fills}
    for p in positions:
        if p["symbol"] not in filled_symbols:
            fail("position_without_fills", symbol=p["symbol"], quantity=p["quantity"])

    seen = set()
    for order in state["orders"]["orders"]:
        oid, qty, filled, remaining = order["id"], int(order["quantity"]), int(order["filled_quantity"]), int(order["remaining_quantity"])
        if oid in seen:
            fail("duplicate_order", order_id=oid)
        seen.add(oid)
        if filled < 0 or filled > qty:
            fail("order_overfill", order_id=oid, quantity=qty, filled=filled)
        if order["status"] in WORKING and (remaining <= 0 or filled >= qty or order.get("ended_at") is not None):
            fail("filled_and_working", order_id=oid, status=order["status"], quantity=qty, filled=filled, remaining=remaining)
        if order["status"] == "filled" and (filled != qty or remaining != 0):
            fail("filled_order_quantity", order_id=oid, quantity=qty, filled=filled, remaining=remaining)
        legs = order.get("legs") or [{"symbol": order["symbol"], "ratio": 1}]
        for leg in legs:
            actual = fill_totals[oid, leg["symbol"]]
            expected = filled * int(leg.get("ratio", 1))
            if actual != expected:
                fail("order_fill_quantity", order_id=oid, symbol=leg["symbol"], actual=actual, expected=expected)

    available = money(portfolio["buying_power"]["available"])
    if available < 0:
        counts["negative_buying_power_samples"] += 1
        if accepted_order and before and account["rules"]["buying_power"]:
            previous = before["portfolio"]
            # Portfolio margin measures free power from equity; strategy from cash.
            basis = "equity" if account["rules"].get("margin") == "portfolio" else "cash"
            free = money(portfolio[basis]) - money(portfolio["buying_power"]["requirement"])
            previous_free = money(previous[basis]) - money(previous["buying_power"]["requirement"])
            if free < previous_free and available <= money(previous["buying_power"]["available"]):
                fail("buying_power", available=str(available), before_available=previous["buying_power"]["available"],
                     free=str(free), before_free=str(previous_free))
    return failures, counts


def tick(underlying, price):
    if underlying in ("SPX", "NDX", "RUT"):
        return Decimal("0.05") if price < 3 else Decimal("0.10")
    if underlying == "XSP":
        return Decimal("0.01") if price < 3 else Decimal("0.05")
    return Decimal("0.01")


def grid_price(underlying, price):
    price = max(Decimal("0.01"), money(price))
    for _ in range(2):  # Rounding can cross the $3 tier boundary.
        unit = tick(underlying, price)
        price = max(unit, (price / unit).to_integral_value(rounding=ROUND_HALF_UP) * unit)
    return format(price, ".2f")


def epoch(value):
    return datetime.fromisoformat(value.replace("Z", "+00:00")).timestamp()


def operation(kind, method, suffix, body=None, preview=False):
    return dict(kind=kind, method=method, path=PREFIX + suffix, body=body, preview=preview)


class Generator:
    def __init__(self, client, rng, replay, ops):
        self.client, self.rng, self.replay, self.ops = client, rng, replay, ops
        self.chains = {}

    def advance(self, step, state):
        now = state["portfolio"]["time"]
        remaining = max(1, int(epoch(self.replay["end"]) - epoch(now)))
        # Budget advances over the whole scenario, including overnight gaps. Small
        # next/seconds steps still exercise boundaries. The final step reaches EOF.
        if step == self.ops:
            until = self.replay["end"]
        elif self.rng.random() < .25:
            until = self.rng.choice(["next", "+1s", "+15s"])
        else:
            budget = max(1, (self.ops - step) * .20)
            minutes = max(1, min(remaining // 60, int(remaining / budget / 60 * self.rng.uniform(.6, 1.2))))
            until = f"+{minutes}m" if remaining >= 60 else f"+{remaining}s"
        return operation("advance", "PUT", "", {"until": until})

    def quotes(self, state):
        now = state["portfolio"]["time"]
        if self.chains.get("time") == now:
            return self.chains["quotes"]
        quotes = []
        for underlying in self.replay["symbols"]:
            summary = get(self.client, PREFIX + f"/underlyings/{underlying}/summary")
            expiries = summary["expiries"]
            if not expiries:
                continue
            # Both the front and a later expiry; allows expiry and carry positions.
            expiry = self.rng.choice(expiries[:3])["id"]
            chain = get(self.client, PREFIX + f"/underlyings/{underlying}/chain?" + urllib.parse.urlencode({"expiry": expiry}))
            for row in chain["strikes"]:
                for kind in ("call", "put"):
                    q = row.get(kind)
                    if q and q.get("bid") is not None and q.get("ask") is not None and money(q["ask"]) > 0:
                        quotes.append(dict(q, underlying=underlying, expiry=expiry, option_type=kind))
        self.chains = dict(time=now, quotes=quotes)
        return quotes

    def draw(self, step, state):
        r, portfolio = self.rng, state["portfolio"]
        kind = r.choices(["advance", "single", "spread", "stop", "bracket", "exit", "modify", "cancel",
                          "stock", "stock_close", "flatten", "abandon", "instruction", "limits", "guardrails"],
                         [25, 22, 9, 6, 8, 5, 6, 5, 5, 3, 3, 1, 2, 1, 1])[0]
        if kind == "advance" or step == self.ops:
            return self.advance(step, state)
        held = portfolio["positions"]
        working = [o for o in state["orders"]["orders"] if o["status"] in WORKING]
        if kind in ("modify", "cancel") and working:
            order = r.choice(working)
            if kind == "cancel":
                return operation(kind, "DELETE", "/orders/" + order["id"])
            body = {"quantity": int(order["filled_quantity"]) + r.randint(1, 4)}
            if order["type"] == "limit" and order["symbol"]:
                body["limit_price"] = grid_price(order["underlying"], money(order["limit_price"]) + r.choice([-1, 1]) * tick(order["underlying"], money(order["limit_price"])))
            return operation(kind, "PUT", "/orders/" + order["id"], body, r.random() < .5)
        if kind == "flatten":
            return operation(kind, "POST", "/positions/close", {})
        if kind in ("abandon", "instruction") and held:
            position = r.choice([p for p in held if p["quantity"] > 0] or held)
            body = {"symbol": position["symbol"]}
            if kind == "instruction":
                body["do_not_exercise"] = r.choice([True, False])
            return operation(kind, "POST", "/positions/" + kind, body)
        if kind in ("limits", "guardrails"):
            risk = get(self.client, PREFIX + "/risk")
            settings = copy.deepcopy(risk[kind])
            if kind == "limits":
                settings["max_order_contracts"] = r.choice([2, 5, 10, 25])
            else:
                settings["cooldown_minutes"] = r.choice([0, 1, 3])
                settings["cooldown_loss"] = r.choice(["0.00", "100.00", "500.00"])
            return operation(kind, "PUT", "/risk/" + kind,
                             {"expected_revision": risk["limits_revision"], kind: settings})
        if kind in ("stock", "stock_close"):
            # Session metadata covers holidays and early closes; shares stop at 16:00
            # (13:00 early), fifteen minutes before the regular options session end.
            now = epoch(portfolio["time"])
            stock_hours = any(s["session"] == "regular" and epoch(s["open"]) <= now < epoch(s["end"]) - 15 * 60
                              for s in self.replay["sessions"])
            symbols = [s for s in self.replay["symbols"] if s in ("SPY", "QQQ")]
            if stock_hours and kind == "stock_close" and portfolio["stocks"]:
                return operation(kind, "POST", "/stocks/close", {"symbol": r.choice(portfolio["stocks"])["symbol"]})
            if stock_hours and symbols:
                return operation("stock", "POST", "/stocks/trade",
                                 {"symbol": r.choice(symbols), "side": r.choice(["buy", "sell"]), "shares": r.choice([1, 10, 100])}, r.random() < .5)
        quotes = self.quotes(state)
        if not quotes:
            return self.advance(step, state)
        # Bias toward liquid, affordable OTM contracts, but retain the full chain.
        liquid = [q for q in quotes if q.get("delta") is not None and .05 <= abs(q["delta"]) <= .55]
        q = r.choice(liquid or quotes)
        side, quantity = r.choice(["buy", "sell"]), r.choice([1, 1, 2, 3, 12])
        positions = {p["symbol"]: p for p in held}
        closing = [q for q in quotes if q["symbol"] in positions]
        if closing and (kind == "exit" or r.random() < .35):
            q = r.choice(closing)
            pos = positions[q["symbol"]]
            side, quantity = ("sell" if pos["quantity"] > 0 else "buy"), r.randint(1, int(abs(pos["quantity"])))
        typ = r.choice(["market", "limit", "limit"])
        body = dict(client_order_id=f"fuzz-{step}", symbol=q["symbol"], side=side, quantity=quantity,
                    type=typ, time_in_force="ioc" if typ == "market" else r.choice(["ioc", "day", "gtc"]))
        price = money(r.choice([q["bid"], q["ask"]])) + r.choice([-1, 0, 1]) * tick(q["underlying"], money(q["ask"]))
        if typ == "limit":
            body["limit_price"] = grid_price(q["underlying"], price)
        actual_kind = "single_" + typ
        if kind == "spread":
            others = [p for p in quotes if p["underlying"] == q["underlying"] and p["expiry"] == q["expiry"]
                      and p["option_type"] == q["option_type"] and p["symbol"] != q["symbol"]]
            if others:
                other = r.choice(others)
                body.pop("symbol")
                body.pop("side")
                body["legs"] = [{"symbol": q["symbol"], "side": side, "ratio": 1},
                                {"symbol": other["symbol"], "side": "sell" if side == "buy" else "buy", "ratio": 1}]
                if typ == "limit":
                    net = (money(q["ask"]) - money(other["bid"])) if side == "buy" else (money(other["ask"]) - money(q["bid"]))
                    unit = tick(q["underlying"], Decimal(0))
                    body["limit_price"] = format((net / unit).to_integral_value(rounding=ROUND_HALF_UP) * unit, ".2f")
                actual_kind = "spread_" + typ
        elif kind in ("stop", "bracket", "exit"):
            exit_only = kind == "exit" and q["symbol"] in positions
            long = (side == "sell") if exit_only else (side == "buy")
            level = grid_price(q["underlying"], money(q["bid"] if long else q["ask"]) * (Decimal("0.8") if long else Decimal("1.2")))
            trigger = {"source": "option", "direction": "at_or_below" if long else "at_or_above", "level": level}
            if kind == "stop":
                body["trigger"] = trigger
            else:
                body["bracket"] = {"stop_loss": {"trigger": trigger}}
                if exit_only:
                    body.update(exits_only=True, type="market", time_in_force="ioc", trigger=trigger)
                    body.pop("limit_price", None)
                elif r.random() < .5:
                    body["bracket"]["take_profit"] = {"limit_price": grid_price(q["underlying"], money(q["ask"]) * (Decimal("1.2") if long else Decimal("0.8")))}
            actual_kind = "exit" if exit_only else ("stop" if kind == "stop" else "bracket")
        return operation(actual_kind, "POST", "/orders", body, r.random() < .5)


def journal_path(entry, directory=None):
    # `file` is a recording identifier. Never interpret it as a journal path.
    exposed = entry.get("journal_path")
    if exposed:
        return Path(exposed)
    if directory:
        run_id = entry["id"]
        if Path(run_id).name != run_id or run_id in (".", ".."):
            raise FuzzError("unsafe run id in history")
        return Path(directory) / (run_id + ".jsonl")
    raise FuzzError("history exposes no journal path; use --replays-dir DIR beside the server's --paper-journal")


def check_history(entry):
    if entry.get("error") or any(entry.get(k) for k in ("torn", "bytes_cut", "truncated", "mismatch")):
        raise FuzzError(f"journal integrity: {entry}")
    for key in ("journal", "journal_found"):
        if not all(k in entry.get(key, {}) for k in ("head", "transactions", "bytes")):
            raise FuzzError(f"missing history {key} checkpoint")
    if entry["journal"] != entry["journal_found"]:
        raise FuzzError(f"journal checkpoint mismatch: {entry['journal']} != {entry['journal_found']}")


def verify(client, run_id, timeout=3600, pause=.2):
    path = PREFIX + "/history/" + urllib.parse.quote(run_id, safe="") + "/verify"
    request(client, "POST", path, {})
    deadline = time.monotonic() + timeout
    while True:
        result = get(client, path)
        if result["status"] in ("passed", "failed"):
            return result
        if result["status"] not in ("idle", "running"):
            raise FuzzError(f"unexpected verification state: {result}")
        if time.monotonic() >= deadline:
            raise FuzzError(f"verification timed out for {run_id}")
        time.sleep(pause)


def compare_runs(runs, directory=None):
    result = {"status": "passed", "normalized_fields": [], "files": "not_requested"}
    a, b = [r["history"] for r in runs]
    result["journals"] = [a["journal"], b["journal"]]
    if a["journal"] != b["journal"]:
        result["status"] = "failed"
    if directory:
        equal = filecmp.cmp(journal_path(a, directory), journal_path(b, directory), shallow=False)
        result["files"] = "identical" if equal else "different"
        if not equal:
            result["status"] = "failed"
    # Different refusals/times must also be visible even if neither reached the journal.
    if runs[0]["outcomes"] != runs[1]["outcomes"]:
        result["status"] = "failed"
        result["outcomes_match"] = False
    return result


def run_once(client, start, ops, fuzz_seed, tape=None, binary=None, directory=None, out=print):
    result = dict(operations=Counter(), refusals=Counter(), preview_refusals=Counter(), checks=Counter(),
                  failures=[], commands=[], outcomes=[], verification={"status": "not_run"},
                  cli_verification={"status": "not_requested"})
    run_id, step, op, start_attempted = None, 0, None, False

    def failure(check, **values):
        item = dict(step=step, operation=copy.deepcopy(op), check=check, **values)
        result["failures"].append(item)
        out("FAIL", dumps(item).strip())

    try:
        request(client, "DELETE", PREFIX)
        # A lost/malformed start response may still have created a replay.
        start_attempted = True
        replay = request(client, "POST", PREFIX, start)[1]["replay"]
        run_id = replay["id"]
        result.update(run_id=run_id, scenario=replay["scenario"], seed=replay["seed"], plan=replay["plan"])
        deadline = time.monotonic() + 3600
        while replay.get("fast_forwarding") or not replay.get("time"):
            if time.monotonic() >= deadline:
                raise FuzzError("replay preparation timed out")
            time.sleep(.1)
            replay = get(client, PREFIX)["replay"]
        if not replay["paused"]:
            raise FuzzError("replay did not start paused")
        state = snapshot(client)
        result["start_market_time"] = state["portfolio"]["time"]
        generator = Generator(client, random.Random(fuzz_seed), replay, ops)
        abandoned = Counter()
        issues, counters = check_invariants(state)
        result["checks"].update(counters)
        for item in issues:
            failure(**item)
        for step in range(1, ops + 1):
            op = copy.deepcopy(tape[step - 1]) if tape is not None else generator.draw(step, state)
            market_time = state["portfolio"]["time"]
            if tape is None:
                op["market_time"] = market_time
            elif op["market_time"] != market_time:
                failure("command_market_time", expected=op["market_time"], actual=market_time)
            result["commands"].append(copy.deepcopy(op))
            result["operations"][op["kind"]] += 1
            preview = None
            if op["preview"]:
                preview = request(client, "POST", op["path"] + "/preview", op["body"], result["preview_refusals"])[1]
                if preview.get("decision") not in (None, "ok"):
                    result["preview_refusals"][preview["decision"]] += 1
            status, response = request(client, op["method"], op["path"], op["body"], result["refusals"])
            accepted = status < 300 and (response.get("order") or op["kind"] == "stock")
            if accepted and preview and (preview.get("decision") == "BUYING_POWER" or
                                         (preview.get("reason") or {}).get("code") == "BUYING_POWER" or error_code(preview) == "BUYING_POWER"):
                failure("preview_buying_power", preview=preview, accepted=response)
            if status < 300 and op["kind"] == "abandon":
                abandoned[op["body"]["symbol"]] += next(p["quantity"] for p in state["portfolio"]["positions"] if p["symbol"] == op["body"]["symbol"])
            before, state = state, snapshot(client)
            result["outcomes"].append(dict(status=status, code=error_code(response), market_time=state["portfolio"]["time"],
                                           order_status=(response.get("order") or {}).get("status")))
            issues, counters = check_invariants(state, before, bool(accepted), abandoned)
            result["checks"].update(counters)
            for item in issues:
                failure(**item)
            if step % 25 == 0:
                out("run", run_id, "step", step, "time", state["portfolio"]["time"], "failures", len(result["failures"]))
        result["end_market_time"] = state["portfolio"]["time"]
    except (FuzzError, KeyError, TypeError, ValueError, InvalidOperation, OSError) as exc:
        failure("execution", message=str(exc))
    finally:
        if start_attempted:
            try:
                request(client, "DELETE", PREFIX)
            except (FuzzError, OSError) as exc:
                failure("stop", message=str(exc))
    if run_id:
        op = None
        try:
            history = get(client, PREFIX + "/history")["history"]
            entry = next((r for r in history if r["id"] == run_id), None)
            if entry is None:
                raise FuzzError(f"run {run_id} missing from history")
            result["history"] = entry
            try:
                check_history(entry)
            except FuzzError as exc:
                failure("journal_integrity", message=str(exc))
            out("verifying", run_id, "via HTTP")
            result["verification"] = verify(client, run_id)
            if result["verification"]["status"] != "passed":
                failure("verification", result=result["verification"])
            if binary:
                path = journal_path(entry, directory)
                out("verifying", run_id, "via --verify-run", path)
                process = subprocess.run([str(binary), "--verify-run", str(path.resolve())],
                                         capture_output=True, text=True, timeout=3600, check=False)
                result["cli_verification"] = dict(status="passed" if process.returncode == 0 else "failed",
                                                  returncode=process.returncode, journal=str(path),
                                                  stdout=process.stdout, stderr=process.stderr)
                if process.returncode:
                    failure("cli_verification", result=result["cli_verification"])
        except (FuzzError, OSError, ValueError, KeyError, subprocess.TimeoutExpired) as exc:
            failure("verification", message=str(exc))
    return result


def fuzz(client, scenario="reversal", seed=81723, ops=300, fuzz_seed=1, plan="practice", repeat=False,
         binary=None, directory=None, out=print):
    start = dict(scenario=scenario, seed=str(seed), plan=plan, paused=True)
    result = dict(url=client.base, scenario=scenario, seed=str(seed), fuzz_seed=fuzz_seed, ops=ops, plan=plan,
                  runs=[], determinism={"status": "not_requested"})
    first = run_once(client, start, ops, fuzz_seed, binary=binary, directory=directory, out=out)
    result["runs"].append(first)
    if repeat:
        result["determinism"] = {"status": "failed", "message": "first run did not complete the command tape"}
        if len(first["commands"]) == ops and "history" in first:
            second = run_once(client, start, ops, fuzz_seed, tape=first["commands"], binary=binary, directory=directory, out=out)
            result["runs"].append(second)
            try:
                result["determinism"] = compare_runs(result["runs"], directory)
            except (FuzzError, OSError, KeyError) as exc:
                result["determinism"] = dict(status="failed", message=str(exc))
    result["exit_status"] = int(any(r["failures"] for r in result["runs"]) or result["determinism"]["status"] == "failed")
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("url")
    parser.add_argument("--scenario", default="reversal")
    parser.add_argument("--seed", type=int, default=81723)
    parser.add_argument("--ops", type=int, default=300)
    parser.add_argument("--fuzz-seed", type=int, default=1)
    parser.add_argument("--plan", default="practice")
    parser.add_argument("--repeat", action="store_true")
    parser.add_argument("--token")
    parser.add_argument("--json", metavar="PATH", help="sorted JSON summary; - writes stdout")
    parser.add_argument("--openportd", metavar="BIN")
    parser.add_argument("--replays-dir", metavar="DIR")
    args = parser.parse_args(argv)
    if args.ops < 1 or not 0 <= args.seed < 2 ** 64:
        parser.error("--ops must be positive and --seed must fit uint64")
    out = lambda *parts: print(*parts, file=sys.stderr if args.json == "-" else sys.stdout, flush=True)
    result = fuzz(Client(args.url, args.token), args.scenario, args.seed, args.ops, args.fuzz_seed, args.plan,
                  args.repeat, args.openportd, args.replays_dir, out)
    for run in result["runs"]:
        out("run", run.get("run_id"), "operations", dict(run["operations"]), "refusals", dict(run["refusals"]),
            "preview refusals", dict(run["preview_refusals"]),
            "checks", dict(run["checks"]), "failures", len(run["failures"]),
            "verify", run["verification"]["status"], "CLI", run["cli_verification"]["status"])
    out("determinism", result["determinism"], "exit", result["exit_status"])
    if args.json:
        try:
            if args.json == "-":
                sys.stdout.write(dumps(result))
            else:
                Path(args.json).write_text(dumps(result), encoding="utf-8")
        except OSError as exc:
            out("FAIL writing summary:", exc)
            return 1
    return result["exit_status"]


if __name__ == "__main__":
    sys.exit(main())
