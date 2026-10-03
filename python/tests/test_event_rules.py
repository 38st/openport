from typing import get_type_hints

import jsonschema
import pytest

from conftest import SPEC
from openport.types import AccountRules, Evaluation, PlanEvent, EventWindow
from tools.contract_test import Contract


def test_calendar_types_and_schemas():
    fields = {"events", "news_before_minutes", "news_after_minutes", "news_action", "hold_restrictions", "hold_cutoff"}
    assert fields <= AccountRules.__optional_keys__
    assert get_type_hints(AccountRules)["events"] == list[PlanEvent]
    assert get_type_hints(Evaluation)["next_event"] == EventWindow | None
    assert {"next_event", "active_events"} <= Evaluation.__optional_keys__
    contract = Contract("", None, SPEC)
    validator = jsonschema.Draft202012Validator({"$ref": "urn:openport#/components/schemas/PlanEvent"}, registry=contract.registry)
    validator.validate({"kind": "news", "time": "2026-09-22T14:00:00Z"})
    for kind in ("earnings", "ex_dividend", "split"):
        validator.validate({"kind": kind, "time": "2026-09-23", "symbol": "SPY"})
    for event in ({"kind": "split", "time": "2026-09-23"},
                  {"kind": "news", "time": "2026-09-22T14:00:00Z", "session": "before_open"},
                  {"kind": "earnings", "time": "2026-09-22", "symbol": "spy"},
                  {"kind": "split", "time": "2026-09-22", "symbol": "SPY", "label": "x" * 65}):
        with pytest.raises(jsonschema.ValidationError):
            validator.validate(event)
    assert "/api/calendar/events" in SPEC["paths"]
