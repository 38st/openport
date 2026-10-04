from typing import get_type_hints
from urllib.parse import urlsplit

import jsonschema
import pytest

from conftest import SPEC, shaped
from openport import Client
from openport.types import AccountRules, Evaluation, SizeScaling, SizeScale, SizeScalingStatus
from tools.contract_test import Contract


def test_size_scaling_types_and_contract():
    for wire in (SizeScaling, SizeScale, SizeScalingStatus):
        assert set(get_type_hints(wire)) == set(SPEC["components"]["schemas"][wire.__name__]["properties"])
    assert get_type_hints(AccountRules)["size_scaling"] == SizeScaling | None
    assert get_type_hints(Evaluation)["size_scaling"] == SizeScalingStatus | None
    assert "size_scaling" in Evaluation.__optional_keys__
    contract = Contract("", None, SPEC)
    validator = jsonschema.Draft202012Validator({"$ref": "urn:openport#/components/schemas/SizeScaling"}, registry=contract.registry)
    rule = dict(profit_percent=10, payouts=2, days=80, increase_percent=25, max_balance="200000.000001")
    validator.validate(rule)
    for key, value in [("profit_percent", 0), ("profit_percent", 101), ("payouts", -1), ("payouts", 101),
                       ("days", 0), ("days", 367), ("days", 1.5), ("increase_percent", 0),
                       ("increase_percent", 101), ("max_balance", 200000), ("unknown", 1)]:
        with pytest.raises(jsonschema.ValidationError):
            validator.validate({**rule, key: value})
    request = jsonschema.Draft202012Validator({"$ref": "urn:openport#/components/schemas/BacktestRequest"}, registry=contract.registry)
    request.validate(dict(playbook="sample", plan=dict(initial_cash="50000.00", rules=dict(phase="funded",
        payouts=dict(qualifying_days=1), size_scaling=rule)), scenarios=1, seed=0))


def test_account_size_scaling_preserves_exact_progress(stub, monkeypatch):
    original = stub.respond
    standing = dict(size="62500.00", original="50000.00",
                    original_max_drawdown="1000.030001", original_daily_loss_limit="500.030001", max_balance="200000.00", period_started="2026-09-24",
                    period_days=12, days_required=80, period_profit="1500.000002", profit_required="6250.00",
                    period_payouts=1, payouts_required=2, next_size="75000.00",
                    history=[dict(day="2026-09-24", old="50000.00", size="62500.00")])
    rule = dict(profit_percent=10, payouts=2, days=80, increase_percent=25, max_balance="200000.00")

    def respond(method, target, headers, body):
        if method == "GET" and urlsplit(target).path == "/api/account":
            account = shaped("Account")
            account["rules"]["size_scaling"] = rule
            account["evaluation"]["size_scaling"] = standing
            return 200, account
        return original(method, target, headers, body)

    monkeypatch.setattr(stub, "respond", respond)
    account = Client(stub.url).account()
    assert account["evaluation"]["size_scaling"] == standing
    assert account["rules"]["size_scaling"] == rule
