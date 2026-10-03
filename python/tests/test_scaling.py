from typing import get_type_hints
from urllib.parse import urlsplit

import jsonschema
import pytest

from conftest import SPEC, shaped
from openport import Client
from openport.types import AccountRules, ScalingStep, ScalingStatus
from tools.contract_test import Contract


def test_scaling_types_and_optional_rules():
    schemas = SPEC["components"]["schemas"]
    for wire in (ScalingStep, ScalingStatus):
        assert set(get_type_hints(wire)) == set(schemas[wire.__name__]["properties"])
    assert get_type_hints(AccountRules)["scaling"] == list[ScalingStep]
    contract = Contract("", None, SPEC)
    validator = jsonschema.Draft202012Validator({"$ref": "urn:openport#/components/schemas/AccountRulesInput"}, registry=contract.registry)
    rules = dict(profit_target=None, max_drawdown=None, drawdown_mode="intraday", buy_only=False,
                 buying_power=False, expiry_cutoff_seconds=0)
    validator.validate(rules)
    validator.validate({**rules, "scaling": [{"profit": "0.00", "contracts": 2}]})
    for steps in (None, [{"profit": 0, "contracts": 2}], [{"profit": "0", "contracts": 0}],
                  [{"profit": "0", "contracts": 10001}], [{"profit": "0", "contracts": 1.5}],
                  [{"profit": "0", "contracts": 2}] * 17):
        with pytest.raises(jsonschema.ValidationError):
            validator.validate({**rules, "scaling": steps})


def test_backtest_contract_accepts_funded_scaling_with_payout_defaults():
    contract = Contract("", None, SPEC)
    validator = jsonschema.Draft202012Validator({"$ref": "urn:openport#/components/schemas/BacktestRequest"}, registry=contract.registry)
    validator.validate({"playbook": "sample", "plan": {"initial_cash": "50000.00", "rules": {
        "phase": "funded", "payouts": {"qualifying_days": 1},
        "scaling": [{"profit": "0.00", "contracts": 2}, {"profit": "1500.00", "contracts": 3}]}},
        "scenarios": 1, "seed": 0})


def test_account_keeps_exact_scaling_progress(stub, monkeypatch):
    original = stub.respond
    steps = [{"profit": "0.00", "contracts": 2}, {"profit": "1500.000001", "contracts": 3}]
    standing = dict(limit=2, held=1, profit="1500.000002", next=steps[1])

    def respond(method, target, headers, body):
        if method == "GET" and urlsplit(target).path == "/api/account":
            account = shaped("Account")
            account["rules"]["scaling"] = steps
            account["evaluation"]["scaling"] = standing
            return 200, account
        return original(method, target, headers, body)

    monkeypatch.setattr(stub, "respond", respond)
    account = Client(stub.url).account()
    assert account["evaluation"]["scaling"] == standing
    assert account["rules"]["scaling"] == steps
