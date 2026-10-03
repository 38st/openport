from typing import get_type_hints

import jsonschema
import pytest

from conftest import SPEC
from openport.types import AccountRules, OrderPreview
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
    from typing import get_args
    from openport.types import PlanRuleReason
    assert set(get_args(PlanRuleReason)) == {"MAX_VOLUME_SHARE", "HEDGING", "COUNTER_POSITION", "TRADE_CONSISTENCY", "MIN_TRADES", "MIN_HOLD", "MICROSCALPING"}
    contract = Contract("", None, SPEC)
    validator = jsonschema.Draft202012Validator(
        {"$ref": "urn:openport#/components/schemas/AccountRulesInput"}, registry=contract.registry)
    base = {"profit_target": None, "max_drawdown": None, "drawdown_mode": "intraday",
            "buy_only": False, "buying_power": True, "expiry_cutoff_seconds": 0}
    for pair in [{"microscalp_seconds": 30}, {"microscalp_percent": 25}]:
        with pytest.raises(jsonschema.ValidationError):
            validator.validate({**base, **pair})


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
