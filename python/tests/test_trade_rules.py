import re
from typing import get_args, get_type_hints

import jsonschema
import pytest

from conftest import ROOT, SPEC
from openport.types import AccountRules, OrderPreview, PlanRuleReason
from tools.contract_test import Contract


def test_trade_rule_wire_types_and_optional_defaults():
    fields = {"max_volume_percent", "no_hedging", "no_counter_positions", "min_hold_seconds", "microscalp_seconds", "microscalp_percent", "min_trades", "trade_consistency_percent", "max_contracts_held", "require_stop_loss", "max_trade_risk", "max_trade_risk_percent"}
    assert fields <= AccountRules.__optional_keys__
    assert get_type_hints(OrderPreview)["trade_risk"] == str | None
    assert get_type_hints(OrderPreview)["trade_risk_limit"] == str | None
    contract = Contract("", None, SPEC)
    validator = jsonschema.Draft202012Validator(
        {"$ref": "urn:openport#/components/schemas/AccountRulesInput"}, registry=contract.registry)
    rules = {"profit_target": None, "max_drawdown": None, "drawdown_mode": "intraday",
             "buy_only": False, "buying_power": True, "expiry_cutoff_seconds": 0}
    validator.validate(rules)
    rules.update(max_volume_percent=25, no_hedging=True, no_counter_positions=True, min_hold_seconds=60, microscalp_seconds=30, microscalp_percent=25, min_trades=12, trade_consistency_percent=40, max_contracts_held=5, require_stop_loss=True, max_trade_risk="100.000001", max_trade_risk_percent=25)
    validator.validate(rules)
    for key, value in [("max_volume_percent", -1), ("max_volume_percent", 101), ("max_volume_percent", 1.5), ("max_volume_percent", True), ("no_hedging", 1), ("no_counter_positions", "true"), ("min_hold_seconds", 3601), ("min_hold_seconds", 0.5), ("microscalp_seconds", 0), ("microscalp_percent", 101), ("min_trades", 10001), ("min_trades", 1.5), ("trade_consistency_percent", 101), ("trade_consistency_percent", 1.5), ("max_contracts_held", 100001), ("max_contracts_held", 1.5),
                       ("require_stop_loss", "true"), ("max_trade_risk", 100), ("max_trade_risk_percent", 101)]:
        with pytest.raises(jsonschema.ValidationError):
            validator.validate({**rules, key: value})


def test_new_reason_codes_and_microscalp_pair():
    expected = {"TIME_LIMIT", "INACTIVITY", "INSTRUMENT_NOT_ALLOWED", "OUTSIDE_PLAN_HOURS", "FLAT_TIME", "OVERNIGHT_HOLD", "SCALING_LIMIT",
                "TRADE_CONSISTENCY", "MIN_TRADES", "MIN_HOLD", "MICROSCALPING", "HEDGING", "COUNTER_POSITION", "MAX_VOLUME_SHARE", "NEWS_BLACKOUT", "HOLD_RESTRICTED"}
    assert set(get_args(PlanRuleReason)) == expected
    typescript = (ROOT / "web/src/api/trading-types.ts").read_text().split("export type PlanRuleReason =", 1)[1].split("export type RuleEvidence", 1)[0]
    assert set(re.findall(r'"([A-Z_]+)"', typescript)) == expected
    contract = Contract("", None, SPEC)
    validator = jsonschema.Draft202012Validator(
        {"$ref": "urn:openport#/components/schemas/AccountRulesInput"}, registry=contract.registry)
    base = {"profit_target": None, "max_drawdown": None, "drawdown_mode": "intraday",
            "buy_only": False, "buying_power": True, "expiry_cutoff_seconds": 0}
    for pair in [{"microscalp_seconds": 30}, {"microscalp_percent": 25}]:
        with pytest.raises(jsonschema.ValidationError):
            validator.validate({**base, **pair})


def test_every_reducer_reason_has_a_wire_code():
    header = (ROOT / "include/openport/trading/types.hpp").read_text()
    enum = header.split("enum class Reason {", 1)[1].split("};", 1)[0]
    codes = re.findall(r"\b[A-Z][A-Z_]+\b", re.sub(r"//[^\n]*", "", enum))
    assert len(codes) == len(set(codes))
    assert f"kLastReason = Reason::{codes[-1]};" in header
    implementation = (ROOT / "src/trading/types.cpp").read_text()
    assert set(re.findall(r"CASE\(([A-Z_]+)\)", implementation)) == set(codes)
    assert set(SPEC["components"]["schemas"]["Decision"]["properties"]["code"]["enum"]) == set(codes)


def test_backtest_schema_accepts_combined_plan_rules():
    rules = {"profit_target": "100.00", "time_limit_days": 30, "inactivity_days": 14, "underlyings": ["SPX"],
             "trading_start": "09:30", "trading_end": "16:00", "flat_time": "15:45", "no_overnight": True,
             "trade_consistency_percent": 40, "min_trades": 12, "min_hold_seconds": 60,
             "microscalp_seconds": 30, "microscalp_percent": 25, "no_hedging": True,
             "no_counter_positions": True, "max_volume_percent": 25, "max_contracts_held": 5,
             "scaling": [{"profit": "0.00", "contracts": 2}, {"profit": "1500.00", "contracts": 3}],
             "size_scaling": None}
    schemas = SPEC["components"]["schemas"]
    validator = jsonschema.Draft202012Validator(
        {"$ref": "urn:openport#/components/schemas/BacktestRequest/properties/plan/oneOf/1/properties/rules"},
        registry=Contract("", None, SPEC).registry)
    validator.validate(rules)
    for name in ["AccountRules", "AccountRulesInput"]:
        assert rules.keys() <= schemas[name]["properties"].keys()
    assert rules.keys() <= AccountRules.__optional_keys__
    for key in ["microscalp_seconds", "microscalp_percent", "trading_start", "trading_end"]:
        with pytest.raises(jsonschema.ValidationError):
            validator.validate({k: v for k, v in rules.items() if k != key})


@pytest.mark.parametrize("code,evidence", [
    ("HEDGING", {"underlying": "SPY", "order_dollar_delta": -500, "held_dollar_delta": 1000}),
    ("COUNTER_POSITION", {"underlying": "SPY", "order_dollar_delta": -500,
                          "held_dollar_delta": 1000, "other_account": "second"}),
    ("MAX_VOLUME_SHARE", {"contract": "SPY   261022C00500000", "contracts": 2,
                         "volume": None, "percent": 10}),
])
def test_direction_and_volume_evidence_wire_contract(code, evidence):
    from openport.types import RuleEvidence
    assert get_type_hints(RuleEvidence)["volume"] == int | None
    contract = Contract("", None, SPEC)
    decision = {"code": code, "message": "Opening refused", "actual": None,
                "limit": None, "scope": "SPY", "evidence": evidence}
    for schema, value in [("Decision", decision), ("Error", {"error": decision})]:
        validator = jsonschema.Draft202012Validator(
            {"$ref": f"urn:openport#/components/schemas/{schema}"}, registry=contract.registry)
        validator.validate(value)
