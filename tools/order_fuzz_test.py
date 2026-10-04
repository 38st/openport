"""Offline order fuzz tests: scripted API responses, no server or network."""
import copy
from contextlib import redirect_stderr, redirect_stdout
from decimal import Decimal
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import urllib.error

sys.path.insert(0, str(Path(__file__).resolve().parent))
import order_fuzz as fuzz

CALL = "SPY   260916C00590000"
PUT = "SPY   260916P00590000"
NOW = "2026-09-16T13:30:00.000Z"
END = "2026-09-16T20:15:00.000Z"


def state():
    power = dict(available="100000.00", requirement="0.00", reserved="0.00")
    return dict(account=dict(rules=dict(buying_power=True, margin="strategy"), evaluation=dict(equity="100000.00")),
                portfolio=dict(time=NOW, cash="100000.00", equity="100000.00", positions=[], stocks=[], buying_power=power),
                fills=dict(fills=[]), orders=dict(orders=[]), trades=dict(stock_fills=[]), settlements=dict(settlements=[]))


def position(symbol=CALL, quantity=1):
    return dict(symbol=symbol, quantity=quantity, mark="1.005001", market_value=str(Decimal("100.5001") * quantity))


def fill(symbol=CALL, quantity=1, side="buy", order_id="1"):
    return dict(id="1", order_id=order_id, symbol=symbol, quantity=quantity, side=side)


def order(**changes):
    result = dict(id="1", symbol=CALL, quantity=1, filled_quantity=1, remaining_quantity=0, status="filled", legs=None)
    result.update(changes)
    return result


def balanced(s):
    p = s["portfolio"]
    p["equity"] = str(Decimal(p["cash"]) + sum(Decimal(x["market_value"]) for x in p["positions"] + p["stocks"]))
    s["account"]["evaluation"]["equity"] = p["equity"]
    return s


def held_state():
    s = state()
    s["portfolio"]["positions"] = [position()]
    s["fills"]["fills"] = [fill()]
    s["orders"]["orders"] = [order()]
    return balanced(s)


class ScriptAPI:
    base = "http://scripted"

    def __init__(self, responses=None, verify_status="passed", corrupt=None):
        self.responses = responses or {}
        self.verify_status, self.corrupt = verify_status, corrupt
        self.calls, self.writes, self.started = [], [], []
        self.run, self.poll, self.current = 0, 0, state()
        self.definitions, self.modes = {}, {}

    def entry(self, run):
        journal = dict(head="abc", transactions=5, bytes=100)
        entry = dict(id=f"run-{run}", journal=journal, journal_found=journal.copy(), file="a recording.oprec")
        if self.corrupt:
            entry[self.corrupt] = True
        return entry

    def call(self, method, path, body=None):
        self.calls.append((method, path, copy.deepcopy(body)))
        if path == "/api/playbooks":
            if method == "POST":
                self.definitions[body["id"]] = dict(versions=[dict(body, version=1)], deleted=False)
            return 200, dict(definitions=copy.deepcopy(self.definitions), modes={})
        assert path.startswith(fuzz.PREFIX), path
        if path == fuzz.PREFIX:
            if method == "DELETE":
                return 200, {"replay": None}
            if method == "POST":
                self.run += 1
                self.current = state()
                self.modes = {}
                self.started.append(copy.deepcopy(body))
            elif method == "PUT":
                self.writes.append((self.run, method, path, copy.deepcopy(body)))
                return 200, {"replay": {"paused": True}}
            return 201 if method == "POST" else 200, {"replay": dict(
                id=f"run-{self.run}", scenario="reversal", plan="practice", seed="81723", time=NOW,
                paused=True, end=END, symbols=["SPY"], sessions=[dict(session="regular", open=NOW, end=END)])}
        if path.endswith("/mode"):
            identifier = path.split("/")[-2]
            self.modes[identifier] = body["mode"]
            self.writes.append((self.run, method, path, copy.deepcopy(body)))
            self.current = held_state()
            self.current["orders"]["orders"][0].update(actor="system", tags=[f"playbook:{identifier}@v1"])
            self.current["fills"]["fills"][0]["actor"] = "system"
            return 200, dict(definitions=copy.deepcopy(self.definitions), modes=copy.deepcopy(self.modes))
        if path == fuzz.PREFIX + "/history":
            return 200, {"history": [self.entry(i) for i in range(1, self.run + 1)]}
        if path.endswith("/verify"):
            if method == "POST":
                self.poll = 0
                return 202, {"status": "running"}
            self.poll += 1
            return 200, {"status": "running" if self.poll == 1 else self.verify_status}
        suffix = path.removeprefix(fuzz.PREFIX + "/")
        if method == "GET" and suffix in self.current:
            return 200, copy.deepcopy(self.current[suffix])
        if path.endswith("/preview"):
            return self.responses.get((self.run, "preview"), (200, dict(decision="ok", reason=None)))
        if method != "GET":
            self.writes.append((self.run, method, path, copy.deepcopy(body)))
            return self.responses.get((self.run, "write"), (422, {"error": {"code": "BUYING_POWER"}}))
        raise AssertionError((method, path, body))


def scripted_run(api, repeat=False, commands=None, **kwargs):
    commands = commands or [fuzz.operation("single_market", "POST", "/orders", dict(client_order_id="a", symbol=CALL, side="buy", quantity=12, type="market", time_in_force="ioc"), True),
                fuzz.operation("cancel", "DELETE", "/orders/17"),
                fuzz.operation("advance", "PUT", "", {"until": "+1m"})]
    with patch.object(fuzz.Generator, "draw", side_effect=commands) as draw, patch.object(fuzz.time, "sleep"):
        result = fuzz.fuzz(api, ops=len(commands), repeat=repeat, out=lambda *x: None, **kwargs)
    return result, draw


class InvariantTest(unittest.TestCase):
    def checks(self, s, **kw):
        return {x["check"] for x in fuzz.check_invariants(s, **kw)[0]}

    def test_exact_money_and_signed_marks(self):
        s = held_state()
        self.assertFalse(self.checks(s))
        s["portfolio"]["equity"] = str(Decimal(s["portfolio"]["equity"]) + Decimal("0.000001"))
        failures, _ = fuzz.check_invariants(s)
        self.assertEqual(failures[0]["difference"], "0.000001")
        s = state()
        s["portfolio"]["positions"] = [position(quantity=-2)]
        s["fills"]["fills"] = [fill(quantity=2, side="sell")]
        s["orders"]["orders"] = [order(quantity=2, filled_quantity=2)]
        self.assertFalse(self.checks(balanced(s)))

    def test_missing_mark_skips_only_equity(self):
        s = held_state()
        s["portfolio"]["positions"][0].update(mark=None, market_value=None, quantity=2)
        failures, counts = fuzz.check_invariants(s)
        self.assertEqual(counts["equity_skipped_missing_marks"], 1)
        self.assertEqual(failures[0]["check"], "option_quantity")

    def test_consistent_equity_cannot_hide_wrong_position_value(self):
        s = held_state()
        s["portfolio"]["positions"][0]["market_value"] = "100.500101"
        failures, _ = fuzz.check_invariants(balanced(s))
        self.assertEqual(len(failures), 1)
        self.assertEqual(failures[0]["check"], "position_market_value")
        self.assertEqual(failures[0]["difference"], "0.000001")

    def test_missing_position_and_unexplained_position(self):
        s = held_state()
        s["portfolio"]["positions"] = []
        self.assertIn("option_quantity", self.checks(balanced(s)))
        s = held_state()
        s["fills"]["fills"] = []
        self.assertTrue({"option_quantity", "position_without_fills", "order_fill_quantity"} <= self.checks(s))

    def test_settlement_and_expiry_delivery_not_double_counted(self):
        for symbol, direction, shares in ((CALL, "sell", -100), (PUT, "sell", 100), (CALL, "buy", 100)):
            s = state()
            s["fills"]["fills"] = [fill(symbol, side=direction)]
            s["settlements"]["settlements"] = [dict(symbol=symbol, quantity=-1 if direction == "sell" else 1, time=NOW)]
            s["trades"]["stock_fills"] = [dict(symbol="SPY", shares=shares, option=symbol, source="assignment" if direction == "sell" else "expiry_exercise", time=NOW)]
            s["portfolio"]["stocks"] = [dict(symbol="SPY", shares=shares, mark="600.00", market_value=str(shares * 600))]
            self.assertFalse(self.checks(balanced(s)))

    def test_partial_early_assignment_and_stock_close(self):
        s = state()
        s["fills"]["fills"] = [fill(quantity=3, side="sell")]
        s["portfolio"]["positions"] = [position(quantity=-2)]
        s["trades"]["stock_fills"] = [dict(symbol="SPY", shares=-100, option=CALL, source="assignment", time=NOW),
                                        dict(symbol="SPY", shares=100, option=None, source="trade", time=NOW)]
        self.assertFalse(self.checks(balanced(s)))
        s["trades"]["stock_fills"].pop()
        self.assertIn("stock_quantity", self.checks(s))

    def test_abandon_and_do_not_exercise(self):
        s = state()
        s["fills"]["fills"] = [fill(quantity=2)]
        self.assertFalse(self.checks(s, abandoned={CALL: 2}))
        s["settlements"]["settlements"] = [dict(symbol=CALL, quantity=2, time=NOW, cash="0.00")]
        self.assertFalse(self.checks(s))

    def test_multileg_ratios_and_overfills(self):
        s = state()
        s["fills"]["fills"] = [fill(quantity=2), fill(PUT, quantity=4, side="sell")]
        s["portfolio"]["positions"] = [position(quantity=2), position(PUT, -4)]
        s["orders"]["orders"] = [order(symbol=None, quantity=2, filled_quantity=2, legs=[dict(symbol=CALL, ratio=1), dict(symbol=PUT, ratio=2)])]
        self.assertFalse(self.checks(balanced(s)))
        s["orders"]["orders"][0].update(status="working", quantity=1)
        self.assertTrue({"order_overfill", "filled_and_working"} <= self.checks(s))

    def test_buying_power_documented_exceptions(self):
        before, after = state(), state()
        after["portfolio"]["buying_power"]["available"] = "-1.00"
        after["orders"]["orders"] = [order(status="working", filled_quantity=0, remaining_quantity=1)]
        self.assertIn("buying_power", self.checks(after, before=before, accepted_order=True))
        after["portfolio"].update(cash="-1.00", equity="-1.00")
        after["account"]["evaluation"]["equity"] = "-1.00"
        after["portfolio"]["buying_power"]["available"] = "-1.00"
        self.assertIn("buying_power", self.checks(after, before=before, accepted_order=True))
        self.assertNotIn("buying_power", self.checks(after, before=before))  # market move
        before["portfolio"].update(cash="-2.00")
        before["portfolio"]["buying_power"]["available"] = "-2.00"
        self.assertNotIn("buying_power", self.checks(after, before=before, accepted_order=True))
        before["portfolio"].update(cash="0.00")
        before["portfolio"]["buying_power"]["available"] = "-1.00"
        self.assertIn("buying_power", self.checks(after, before=before, accepted_order=True))
        after["account"]["rules"]["buying_power"] = False
        self.assertNotIn("buying_power", self.checks(after, before=state(), accepted_order=True))


class FlowTest(unittest.TestCase):
    def test_playbook_is_created_once_and_enabled_identically_on_each_replay(self):
        api = ScriptAPI()
        result, _ = scripted_run(api, repeat=True, playbook=True)
        self.assertEqual(result["exit_status"], 0)
        creates = [c for c in api.calls if c[:2] == ("POST", "/api/playbooks")]
        self.assertEqual(len(creates), 1)
        self.assertLess(api.calls.index(creates[0]), next(i for i, c in enumerate(api.calls) if c[:2] == ("POST", fuzz.PREFIX)))
        self.assertEqual(creates[0][2]["management"], {"close_by": "09:32"})
        modes = [c for c in api.calls if c[1].endswith("/mode")]
        self.assertEqual(modes, [("PUT", "/api/replay/playbooks/order-fuzz-1/mode", {"mode": "auto"})] * 2)
        a, b = result["runs"]
        self.assertEqual(a["playbook"], b["playbook"])
        self.assertEqual(a["actors"]["playbook"]["orders_by_actor"], {"system": 1})
        self.assertEqual(a["actors"]["playbook"]["fills_by_actor"], {"system": 1})

    def test_playbook_requires_actual_auto_fills_and_identical_definition(self):
        class BadPlaybook(ScriptAPI):
            def call(self, method, path, body=None):
                status, response = super().call(method, path, body)
                if path.endswith("/mode"):
                    self.current = state()
                    if self.run == 2:
                        response["definitions"][body_id]["versions"][-1]["version"] = 2
                return status, response
        body_id = "order-fuzz-1"
        result, _ = scripted_run(BadPlaybook(), repeat=True, playbook=True)
        self.assertEqual(result["exit_status"], 1)
        self.assertEqual(result["runs"][0]["failures"][0]["check"], "playbook_coverage")
        self.assertIn("differs from setup", result["runs"][1]["failures"][0]["message"])

    def test_actor_counts_link_fills_to_tagged_orders_not_all_system_orders(self):
        s = held_state()
        s["orders"]["orders"][0].update(actor="system", tags=["playbook:example@v1"],
                                         note="Playbook automatic time stop; entry 9")
        s["orders"]["orders"].append(order(id="2", actor="system", tags=[]))
        s["fills"]["fills"] = [dict(fill(), actor="system"), dict(fill(order_id="2"), actor="system")]
        counts = fuzz.actor_counts(s, dict(id="example", version=1))
        self.assertEqual(counts["orders"], {"system": 2})
        self.assertEqual(counts["playbook"]["fills"], 1)
        self.assertEqual(counts["playbook"]["time_stop_orders"], 1)

    def test_refusals_repeat_exact_tape_and_pause(self):
        api = ScriptAPI()
        result, draw = scripted_run(api, repeat=True)
        self.assertEqual(result["exit_status"], 0)
        self.assertEqual(draw.call_count, 3)  # repeat never redraws
        self.assertEqual(result["runs"][0]["commands"], result["runs"][1]["commands"])
        self.assertEqual([x[1:] for x in api.writes if x[0] == 1], [x[1:] for x in api.writes if x[0] == 2])
        self.assertEqual(api.started, [dict(scenario="reversal", seed="81723", plan="practice", paused=True)] * 2)
        self.assertEqual(result["runs"][0]["refusals"], {"BUYING_POWER": 2})
        self.assertEqual(result["runs"][0]["checks"]["samples"], 4)
        self.assertEqual(result["determinism"]["normalized_fields"], [])

    def test_changed_refusal_does_not_change_commands(self):
        api = ScriptAPI({(2, "write"): (409, {"error": {"code": "STOP_REQUIRED"}})})
        result, draw = scripted_run(api, repeat=True)
        self.assertEqual(draw.call_count, 3)
        self.assertEqual(result["runs"][0]["commands"], result["runs"][1]["commands"])
        self.assertEqual(result["exit_status"], 1)
        self.assertFalse(result["determinism"]["outcomes_match"])

    def test_uncertain_start_still_stops_replay(self):
        class MalformedStart(ScriptAPI):
            def call(self, method, path, body=None):
                reply = super().call(method, path, body)
                return (201, {}) if method == "POST" and path == fuzz.PREFIX else reply
        api = MalformedStart()
        result, _ = scripted_run(api)
        self.assertEqual(result["exit_status"], 1)
        self.assertEqual(sum(method == "DELETE" and path == fuzz.PREFIX for method, path, _ in api.calls), 2)

    def test_accepted_insufficient_preview_is_failure(self):
        api = ScriptAPI({(1, "write"): (201, {"order": {"status": "working"}}),
                         (1, "preview"): (200, {"decision": "BUYING_POWER", "reason": {"code": "BUYING_POWER"}})})
        result, _ = scripted_run(api)
        self.assertEqual(result["runs"][0]["failures"][0]["check"], "preview_buying_power")
        self.assertEqual(result["exit_status"], 1)

    def test_5xx_uncoded_4xx_and_connection_failures(self):
        for response in [(500, {"error": {"code": "INTERNAL"}}), (422, {}), (302, {})]:
            result, _ = scripted_run(ScriptAPI({(1, "write"): response}))
            self.assertEqual(result["exit_status"], 1)
            self.assertEqual(result["runs"][0]["verification"]["status"], "passed")
        with patch.object(fuzz.urllib.request, "urlopen", side_effect=urllib.error.URLError("offline")):
            with self.assertRaises(fuzz.FuzzError):
                fuzz.Client("http://offline").call("GET", "/api/replay")

    def test_integrity_verify_failure_and_timeout(self):
        for flag in ("torn", "mismatch", "truncated", "bytes_cut"):
            result, _ = scripted_run(ScriptAPI(corrupt=flag))
            self.assertEqual(result["runs"][0]["failures"][0]["check"], "journal_integrity")
        result, _ = scripted_run(ScriptAPI(verify_status="failed"))
        self.assertEqual(result["exit_status"], 1)
        with patch.object(fuzz.time, "monotonic", side_effect=[0, 2]):
            with self.assertRaisesRegex(fuzz.FuzzError, "timed out"):
                fuzz.verify(ScriptAPI(), "run-1", timeout=1, pause=0)

    def test_cli_exit_status_and_missing_path(self):
        completed = subprocess.CompletedProcess([], 1, "", "mismatch")
        with patch.object(fuzz.subprocess, "run", return_value=completed) as run:
            result, _ = scripted_run(ScriptAPI(), binary="openportd", directory="/tmp/replays")
        self.assertEqual(run.call_args.args[0][0], "openportd")
        self.assertEqual(run.call_args.args[0][-2:], ["--verify-run", str(Path("/tmp/replays/run-1.jsonl").resolve())])
        self.assertEqual(result["exit_status"], 1)
        result, _ = scripted_run(ScriptAPI(), binary="build/openportd")
        self.assertIn("--replays-dir", result["runs"][0]["failures"][0]["message"])

    def test_raw_file_comparison_and_checkpoint_difference(self):
        with tempfile.TemporaryDirectory() as d:
            api = ScriptAPI()
            runs = [dict(history=api.entry(i), outcomes=[]) for i in (1, 2)]
            for i in (1, 2):
                Path(d, f"run-{i}.jsonl").write_bytes(b'{"same":true}\n')
            self.assertEqual(fuzz.compare_runs(runs, d)["status"], "passed")
            changed = Path(d, "run-2.jsonl")
            original = changed.stat()
            changed.write_bytes(b'{"same":null}\n')  # Same size and timestamps, different bytes.
            os.utime(changed, ns=(original.st_atime_ns, original.st_mtime_ns))
            self.assertEqual(fuzz.compare_runs(runs, d)["files"], "different")
        runs[1]["history"]["journal"]["head"] = "other"
        self.assertEqual(fuzz.compare_runs(runs)["status"], "failed")

    def test_cli_json_is_sorted_token_not_persisted(self):
        api = ScriptAPI()
        stdout, stderr = io.StringIO(), io.StringIO()
        with patch.object(fuzz, "Client", return_value=api), patch.object(fuzz.Generator, "draw", return_value=fuzz.operation("advance", "PUT", "", {"until": "next"})), patch.object(fuzz.time, "sleep"), redirect_stdout(stdout), redirect_stderr(stderr):
            status = fuzz.main([api.base, "--ops", "1", "--token", "secret-value", "--json", "-"])
        self.assertEqual(status, 0)
        report = json.loads(stdout.getvalue())
        self.assertEqual(stdout.getvalue(), fuzz.dumps(report))
        self.assertNotIn("secret-value", stdout.getvalue() + stderr.getvalue())


class TransportAndGeneratorTest(unittest.TestCase):
    def test_json_parse_errors_and_bearer_header(self):
        for raw in (b"", b"not JSON", b"null", b"[]", b'{"n": NaN}'):
            response = io.BytesIO(raw)
            response.code = 200
            with patch.object(fuzz.urllib.request, "urlopen", return_value=response) as urlopen:
                with self.assertRaises(fuzz.FuzzError):
                    fuzz.Client("http://fake", "test-token").call("POST", "/api/replay", {})
                self.assertEqual(urlopen.call_args.args[0].get_header("Authorization"), "Bearer test-token")
        response = urllib.error.HTTPError("http://fake", 422, "rule", {}, io.BytesIO(b'{"error":{"code":"BUYING_POWER"}}'))
        with patch.object(fuzz.urllib.request, "urlopen", side_effect=response):
            self.assertEqual(fuzz.Client("http://fake").call("POST", "/api/replay")[0], 422)

    def test_generator_covers_order_families_on_replay_only(self):
        class QuotesAPI(ScriptAPI):
            def call(self, method, path, body=None):
                assert path.startswith(fuzz.PREFIX)
                if path.endswith("/risk"):
                    return 200, dict(limits_revision="1", limits=dict(max_order_contracts=100),
                                     guardrails=dict(cooldown_minutes=0, cooldown_loss="0.00"))
                if path.endswith("/summary"):
                    return 200, {"expiries": [{"id": "2026-09-16PM"}]}
                if "/chain?" in path:
                    return 200, {"strikes": [
                        {"call": dict(symbol=CALL, bid=1.0, ask=1.1, delta=.3),
                         "put": dict(symbol=PUT, bid=1.0, ask=1.1, delta=-.3)},
                        {"call": dict(symbol="SPY   260916C00595000", bid=.5, ask=.6, delta=.2)}]}
                return super().call(method, path, body)
        api = QuotesAPI()
        replay = api.call("POST", fuzz.PREFIX, {})[1]["replay"]
        s = held_state()
        s["portfolio"]["stocks"] = [dict(symbol="SPY", shares=10)]
        s["orders"]["orders"] = [order(status="working", filled_quantity=0, remaining_quantity=1,
                                           type="limit", limit_price="1.00", underlying="SPY")]
        def generate():
            generator = fuzz.Generator(api, fuzz.random.Random(12), replay, 2000)
            return [generator.draw(i, s) for i in range(1, 2001)]
        tape = generate()
        self.assertEqual(tape, generate())
        kinds = {o["kind"] for o in tape}
        self.assertTrue({"single_market", "single_limit", "spread_market", "spread_limit", "stop", "bracket",
                         "exit", "modify", "cancel", "stock", "stock_close", "flatten", "abandon",
                         "instruction", "limits", "guardrails", "advance"} <= kinds)
        self.assertEqual(tape[-1]["body"], {"until": END})
        self.assertTrue(all(o["path"].startswith(fuzz.PREFIX) for o in tape))
        for op in tape:
            body = op["body"] or {}
            if body.get("type") == "market" and "trigger" not in body:
                self.assertEqual(body["time_in_force"], "ioc")

    def test_ticks_and_tier_crossing(self):
        for underlying, price, expected in [("SPX", "2.99", "3.00"), ("SPX", "3.06", "3.10"),
                                             ("XSP", "3.02", "3.00"), ("SPY", "1.234", "1.23"),
                                             ("VIX", "0", "0.01")]:
            self.assertEqual(fuzz.grid_price(underlying, price), expected)


if __name__ == "__main__":
    unittest.main()
