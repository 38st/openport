from typing import get_type_hints

import jsonschema
import pytest

from conftest import SPEC
from openport.types import AccountRules, Order, OrderPreview, Walk, WalkStep
from tools.contract_test import Contract


def test_walk_wire_types_and_optional_defaults():
    schemas = SPEC["components"]["schemas"]
    for wire in (Walk, WalkStep):
        assert set(get_type_hints(wire)) == set(schemas[wire.__name__]["properties"])
    assert get_type_hints(Order)["walk"] == Walk | None
    assert "walk" in Order.__optional_keys__
    assert get_type_hints(OrderPreview)["next_walk"] == WalkStep | None
    assert "inside_fill_percent" in AccountRules.__optional_keys__


def test_walk_changes_and_rules_schema():
    contract = Contract("", None, SPEC)
    def validate(name, body):
        jsonschema.Draft202012Validator({"$ref": f"urn:openport#/components/schemas/{name}"}, registry=contract.registry).validate(body)
    validate("OrderChange", {"walk": None})
    validate("OrderChange", {"walk": {"step": "0.05", "seconds": 5, "limit": "-0.20"}})
    with pytest.raises(jsonschema.ValidationError):
        validate("OrderChange", {"walk": {"step": "0.05", "seconds": 0, "limit": "1.00"}})
    validate("ResetRequest", {"reason": "inside", "plan": "practice", "fill_model": "midpoint"})
