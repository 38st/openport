import json
import os
import subprocess
from typing import get_type_hints

import jsonschema
import pytest

from conftest import ROOT, SPEC
from openport.types import Account, AccountRules, Evaluation
from tools.contract_test import Contract


def test_time_rules_types_and_request_schemas():
    fields = {"time_limit_days", "inactivity_days", "underlyings", "trading_start", "trading_end"}
    assert fields <= AccountRules.__optional_keys__
    hints = get_type_hints(AccountRules)
    assert hints["time_limit_days"] is int
    assert hints["inactivity_days"] is int
    assert hints["underlyings"] == list[str]
    assert hints["trading_start"] == hints["trading_end"] == str | None
    contract = Contract("", None, SPEC)
    rules = {"profit_target": None, "max_drawdown": None, "drawdown_mode": "intraday",
             "buy_only": False, "buying_power": True, "expiry_cutoff_seconds": 0,
             "max_contracts_held": 5, "require_stop_loss": True,
             "max_trade_risk": "123.456789", "max_trade_risk_percent": 25}
    for path in ("AccountRulesInput", "BacktestRequest/properties/plan/oneOf/1/properties/rules"):
        validator = jsonschema.Draft202012Validator(
            {"$ref": "urn:openport#/components/schemas/" + path}, registry=contract.registry)
        # The backtest API uses expiry_cutoff in nanoseconds instead of seconds.
        base = rules if path == "AccountRulesInput" else {**{k: v for k, v in rules.items() if k != "expiry_cutoff_seconds"},
                                                              "profit_target": "100.00", "max_drawdown": "1000.00"}
        validator.validate(base)
        enabled = {**base, "time_limit_days": 366, "inactivity_days": 1,
                   "underlyings": ["SPX", "BRK.B"], "trading_start": "00:00", "trading_end": "24:00"}
        validator.validate(enabled)
        validator.validate({**enabled, "phase": "funded", "time_limit_days": 0, "profit_target": "0.00",
                            "payouts": {"qualifying_profit": "0.00", "qualifying_days": 1, "withdrawal_percent": 50,
                                        "split_percent": 80, "minimum": "0.00", "caps": []}})
        for patch in ({"time_limit_days": 367}, {"inactivity_days": -1}, {"inactivity_days": 1.5},
                      {"underlyings": ["spx"]}, {"underlyings": ["SPX", "SPX"]},
                      {"underlyings": ["X" * 13]}, {"underlyings": [f"S{i}" for i in range(33)]},
                      {"trading_start": "24:00"}, {"trading_end": "24:01"},
                      {"trading_end": None}, {"trading_start": None}, {"phase": "funded"}):
            with pytest.raises(jsonschema.ValidationError):
                validator.validate({**enabled, **patch})


def test_time_progress_schema_bounds_and_reason_documentation():
    props = SPEC["components"]["schemas"]["Evaluation"]["properties"]
    assert get_type_hints(Account)["evaluation"] is Evaluation
    assert props.keys() == Evaluation.__required_keys__ | Evaluation.__optional_keys__
    hints = get_type_hints(Evaluation)
    for field in ("time_limit_days", "deadline", "days_left", "last_activity", "inactive_days", "inactivity_deadline"):
        jsonschema.validate(None, props[field])
        assert field in Evaluation.__optional_keys__
        assert hints[field] == (int | None if field in ("time_limit_days", "days_left", "inactive_days") else str | None)
    for field in ("days_left", "inactive_days"):
        jsonschema.validate(0, props[field])
        with pytest.raises(jsonschema.ValidationError):
            jsonschema.validate(-1, props[field])
    assert "TIME_LIMIT" in props["decision_code"]["description"]
    assert "INACTIVITY" in props["decision_code"]["description"]


def test_read_back_rules_remain_valid_create_and_reset_inputs():
    schemas = SPEC["components"]["schemas"]
    # The editor sends all the account's existing rule fields back with its edits.
    assert schemas["AccountRules"]["properties"].keys() <= schemas["AccountRulesInput"]["properties"].keys()
    assert schemas["AccountRules"]["properties"].keys() <= AccountRules.__optional_keys__
    rules = {"profit_target": "100.00", "max_drawdown": "1000.00", "drawdown_mode": "static",
             "buy_only": False, "buying_power": True, "expiry_cutoff_seconds": 0,
             "phase": "evaluation", "day_end": "18:00", "lock_at_start": False,
             "profit_basis": "balance", "daily_loss_limit": "100.00", "daily_loss_basis": "equity",
             "daily_loss_action": "lock", "consistency_basis": "total", "consistency_percent": 25,
             "min_trading_days": 3, "min_profitable_days": 2, "profitable_day_profit": "10.00",
             "account_type": "margin", "house_margin_percent": 0, "pm_vol_shock": 10,
             "time_limit_days": 30, "inactivity_days": 14, "underlyings": ["SPX"],
             "trading_start": "09:30", "trading_end": "16:00", "max_contracts_held": 5,
             "require_stop_loss": True, "max_trade_risk": "123.456789", "max_trade_risk_percent": 25}
    contract = Contract("", None, SPEC)
    for name, extra in (("CreateAccountRequest", {"name": "Combined"}), ("ResetRequest", {"reason": "Edit"})):
        validator = jsonschema.Draft202012Validator(
            {"$ref": "urn:openport#/components/schemas/" + name}, registry=contract.registry)
        validator.validate({**extra, "initial_cash": "10000.00", "rules": rules})


def test_real_time_rule_create_reset_and_error_responses(tmp_path):
    binary = ROOT / "build/tests/openport_tests"
    if not binary.exists():
        pytest.skip("Build C++ tests to check real time-rule responses")
    result = subprocess.run(
        [str(binary), "--gtest_filter=PaperEngine.TimeRulesRoundTripProgressRefusalsAndValidation:"
         "PaperEngine.TimeAndTradeRulesSurviveCreateResetAndPresetMatching"],
        env={**os.environ, "OPENPORT_CONTRACT_OUTPUT": str(tmp_path)},
        capture_output=True, text=True, timeout=120)
    assert result.returncode == 0, result.stdout + result.stderr
    contract = Contract("", None, SPEC)
    rows = [json.loads(line) for line in (tmp_path / "time-rules.jsonl").read_text().splitlines()]
    for row in rows:
        contract.validate(**row)
    assert {row["status"] for row in rows} == {200, 201, 422}
