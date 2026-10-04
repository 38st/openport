from typing import get_args, get_type_hints

import jsonschema
import pytest

from conftest import SPEC, shaped
from openport import Client
from openport.types import AccountRules, PlanReason, ProgramCosts, ResetReason
from tools.contract_test import Contract


def test_program_cost_wire_types_and_verification_rules():
    fields = {"evaluation_fee", "reset_fee", "activation_fee", "max_resets"}
    assert fields <= AccountRules.__optional_keys__
    assert get_type_hints(ProgramCosts)["resets_left"] == int | None
    contract = Contract("", None, SPEC)
    validator = jsonschema.Draft202012Validator(
        {"$ref": "urn:openport#/components/schemas/AccountRulesInput"}, registry=contract.registry)
    rules = {"profit_target": "1250.00", "max_drawdown": "2000.00", "drawdown_mode": "static",
             "buy_only": False, "buying_power": True, "expiry_cutoff_seconds": 300,
             "phase": "verification", "evaluation_fee": "100.000001", "reset_fee": "25.000001",
             "activation_fee": "0", "max_resets": 2,
             "time_limit_days": 30, "inactivity_days": 14, "underlyings": ["SPX"],
             "trading_start": "09:30", "trading_end": "16:00", "flat_time": "15:45", "no_overnight": True}
    validator.validate(rules)
    for key, value in [("phase", "step3"), ("max_resets", -1), ("max_resets", 1.5), ("reset_fee", 50),
                       ("time_limit_days", 1.5), ("inactivity_days", 367), ("no_overnight", 1)]:
        with pytest.raises(jsonschema.ValidationError):
            validator.validate({**rules, key: value})
    for name, declared in (("PlanReason", PlanReason), ("ResetReason", ResetReason)):
        assert set(get_args(declared)) == set(SPEC["components"]["schemas"][name]["enum"])


def test_cost_settings_pass_through_create_and_reset(stub):
    original = stub.respond
    def respond(method, target, headers, body):
        original(method, target, headers, body)
        return 200, shaped("Account" if "/reset" in target else "CreateAccountResponse")
    stub.respond = respond
    client = Client(stub.url)
    settings = {"plan": "two-step-25k", "evaluation_fee": "100.000001", "reset_fee": "25.000001",
                "activation_fee": "50.00", "max_resets": 2}
    client.reset_account("cost test", **settings)
    assert stub.requests[-1][3] == {"reason": "cost test", **settings}
    client.create_account("Challenge", **settings)
    assert stub.requests[-1][3] == {"name": "Challenge", **settings}


def test_verification_backtests_keep_scaling_and_program_fee_contracts():
    contract = Contract("", None, SPEC)
    validator = jsonschema.Draft202012Validator(
        {"$ref": "urn:openport#/components/schemas/BacktestRequest"}, registry=contract.registry)
    rules = {"phase": "verification", "profit_target": "2500.00", "time_limit_days": 30,
             "scaling": [{"profit": "0.00", "contracts": 2}], "size_scaling": None,
             "evaluation_fee": "100.000001", "reset_fee": "25.000002", "activation_fee": "50.000003", "max_resets": 2}
    request = {"playbook": "sample", "scenarios": 1, "seed": 0, "plan": {"initial_cash": "50000.00", "rules": rules}}
    validator.validate(request)
    for key, value in [("reset_fee", 25), ("evaluation_fee", "-1"), ("max_resets", 1.5),
                       ("phase", "unknown"), ("payouts", {"qualifying_days": 1}),
                       ("size_scaling", {"profit_percent": 1, "payouts": 0, "days": 1,
                                         "increase_percent": 25, "max_balance": "100000"})]:
        with pytest.raises(jsonschema.ValidationError):
            validator.validate({**request, "plan": {**request["plan"], "rules": {**rules, key: value}}})
