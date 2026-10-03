from typing import get_type_hints

import jsonschema
import pytest

from conftest import SPEC
from openport.types import AccountRules, OrderPreview
from tools.contract_test import Contract


def test_trade_rule_wire_types_and_optional_defaults():
    fields = {"max_contracts_held", "require_stop_loss", "max_trade_risk", "max_trade_risk_percent"}
    assert fields <= AccountRules.__optional_keys__
    assert get_type_hints(OrderPreview)["trade_risk"] == str | None
    assert get_type_hints(OrderPreview)["trade_risk_limit"] == str | None
    contract = Contract("", None, SPEC)
    validator = jsonschema.Draft202012Validator(
        {"$ref": "urn:openport#/components/schemas/AccountRulesInput"}, registry=contract.registry)
    rules = {"profit_target": None, "max_drawdown": None, "drawdown_mode": "intraday",
             "buy_only": False, "buying_power": True, "expiry_cutoff_seconds": 0}
    validator.validate(rules)
    rules.update(max_contracts_held=5, require_stop_loss=True, max_trade_risk="100.000001", max_trade_risk_percent=25)
    validator.validate(rules)
    for key, value in [("max_contracts_held", 100001), ("max_contracts_held", 1.5),
                       ("require_stop_loss", "true"), ("max_trade_risk", 100), ("max_trade_risk_percent", 101)]:
        with pytest.raises(jsonschema.ValidationError):
            validator.validate({**rules, key: value})
