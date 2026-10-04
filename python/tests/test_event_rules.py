from typing import get_type_hints

import jsonschema
import pytest

from conftest import SPEC
from openport.types import AccountRules, Evaluation, PlanEvent, EventWindow
from tools.contract_test import Contract


@pytest.mark.parametrize("code,actual,limit,scope", [
    ("SCALING_LIMIT", 3, 2, "aggregate"),
    ("NEWS_BLACKOUT", "2026-09-22T14:00:00Z", "2026-09-22T14:10:00Z", "account"),
    ("HOLD_RESTRICTED", None, "15:45", "split:SPY"),
])
def test_scaling_and_calendar_evidence_share_response_schemas(code, actual, limit, scope):
    decision = {"code": code, "message": "Opening restricted", "actual": actual, "limit": limit, "scope": scope}
    for name, body in (("Decision", decision), ("Error", {"error": decision})):
        jsonschema.Draft202012Validator(SPEC["components"]["schemas"][name]).validate(body)


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
