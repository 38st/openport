# Copyright (c) 2026 OpenPort contributors. MIT License.
import asyncio
import json

import pytest
from openport import Client
from openport_mcp import create_server
from conftest import TIME


def call(server, name, **arguments):
    result = asyncio.run(server.call_tool(name, arguments))
    # MCP 2.x serializes the tool result as structured content and JSON text.
    return json.loads(result.content[0].text)


def test_tool_schemas_name_accounts_and_typed_orders(stub):
    server = create_server(Client(stub.url, "secret"))
    tools = {tool.name: tool for tool in asyncio.run(server.list_tools())}
    assert set(tools) == {"status", "symbols", "summary", "chain", "volatility", "exposure", "account", "positions", "orders", "risk", "preview_order", "place_order", "cancel_order", "flatten", "list_scenarios", "start_replay", "step_replay", "stop_replay"}
    for name in ["place_order", "preview_order", "cancel_order", "flatten", "start_replay", "step_replay", "stop_replay"]:
        assert "account" in tools[name].input_schema["required"]
    schema = tools["place_order"].input_schema
    assert schema["$defs"]["Order"]["properties"]["quantity"]["type"] == "integer"
    assert "limit" in schema["$defs"]["Order"]["properties"]["type"]["enum"]


def test_results_have_market_time_delay_simulation_and_agent_tag(stub):
    server = create_server(Client(stub.url, "secret"), "research")
    result = call(server, "chain", symbol="SPX")
    assert result["as_of"] == TIME
    assert result["simulated"] is True
    assert result["feed"]["delayed"] is True
    assert "window=0.03" in stub.requests[-1][1]
    order = {"quantity": 1, "type": "limit", "time_in_force": "gtc", "symbol": "SPXW  261022P05000000", "side": "buy", "limit_price": "0.05"}
    result = call(server, "place_order", account="practice", order=order)
    assert result["account"] == "practice"
    assert result["paper"] is True
    assert stub.requests[-1][3]["tags"] == ["agent:research"]
    assert stub.requests[-1][3]["client_order_id"]
    assert "account=practice" in stub.requests[-1][1]


def test_write_tools_require_token_even_on_loopback(stub):
    server = create_server(Client(stub.url))
    result = call(server, "flatten", account="main")
    assert result["error"]["code"] == "WRITE_TOKEN_REQUIRED"
    assert all(request[0] == "GET" for request in stub.requests)
    assert result["as_of"] == TIME


def test_missing_quote_time_is_not_replaced_by_another_status_timestamp(stub):
    stub.chain["as_of"] = None
    result = call(create_server(Client(stub.url)), "chain", symbol="SPX")
    assert result["as_of"] is None
    assert result["simulated"] is True


def test_replay_metadata_and_window_bounds(stub):
    server = create_server(Client(stub.url, "secret"))
    assert call(server, "chain", symbol="SPX", window=0)["error"]["code"] == "INVALID_WINDOW"
    assert call(server, "start_replay", account="other", scenario="fixture")["error"]["code"] == "INVALID_ACCOUNT"
    assert call(server, "start_replay", account="main", scenario="fixture")["simulated"] is True
    assert call(server, "step_replay", account="main", until="15:00")["as_of"] == TIME
    assert call(server, "stop_replay", account="main")["data"]["replay"] is None


@pytest.mark.parametrize("name", ["BAD", "../agent", "a" * 27, ""])
def test_agent_names_fit_server_tags(name):
    with pytest.raises(ValueError): create_server(Client(), name)


def test_replay_receipt_clock_is_not_reported_as_market_time(stub):
    server = create_server(Client(stub.url, "secret"))
    result = call(server, "start_replay", account="main", file="recording.oprec")
    assert result["as_of"] == TIME
    stub.replay["time"] = "2026-09-22T14:15:00Z"
    result = call(server, "step_replay", account="main", until="10:15")
    assert result["data"]["replay"]["time"] != TIME
    assert result["as_of"] == TIME
    assert call(server, "stop_replay", account="practice")["error"]["code"] == "INVALID_ACCOUNT"
    assert stub.replay is not None
