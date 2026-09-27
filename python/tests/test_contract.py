# Copyright (c) 2026 OpenPort contributors. MIT License.
import copy

import jsonschema
import pytest
from conftest import SPEC
from tools.contract_test import Contract, ContractMismatch


def test_schema_references_and_required_types():
    for schema in SPEC["components"]["schemas"].values():
        jsonschema.Draft202012Validator.check_schema(schema)
    def walk(value):
        if isinstance(value, dict):
            if "$ref" in value:
                Contract("", None, SPEC).resolve(value)
            for item in value.values(): walk(item)
        elif isinstance(value, list):
            for item in value: walk(item)
    walk(SPEC)


def test_entire_contract_against_threaded_synthetic_http_server(stub):
    contract = Contract(stub.url, "secret", SPEC)
    contract.run()
    expected = {(path, "get") for path, item in SPEC["paths"].items()
                if path.startswith("/api/") and "get" in contract.resolve(item)}
    assert expected <= contract.checked


def test_first_mismatch_names_route_and_field(stub):
    contract = Contract(stub.url, None, SPEC)
    bad = copy.deepcopy(stub.chain)
    bad["strikes"][0]["put"]["bid"] = "not a number"
    with pytest.raises(ContractMismatch, match=r"GET /api/underlyings/SPX/chain: strikes.0.put"):
        contract.validate("GET", "/api/underlyings/SPX/chain", 200, bad)
    bad = copy.deepcopy(stub.chain)
    del bad["expiry"]
    with pytest.raises(ContractMismatch, match="expiry"):
        contract.validate("GET", "/api/underlyings/SPX/chain", 200, bad)


def test_refuses_to_replace_an_active_replay(stub):
    stub.replay = {"existing": True}
    # A malformed response also fails before any write.
    with pytest.raises(ContractMismatch):
        Contract(stub.url, "secret", SPEC).run()
    assert all(method == "GET" for method, *_rest in stub.requests)


def test_real_cpp_handler_responses_without_network(tmp_path):
    import json
    import os
    import subprocess
    from conftest import ROOT
    binary = ROOT / "build/tests/openport_tests"
    if not binary.exists():
        pytest.skip("Build the C++ test executable to check its real handler responses")
    result = subprocess.run([str(binary), "--gtest_filter=Api.ContractFixture:PaperEngine.ContractFixture:ReplayRun.ContractFixture"],
                            env={**os.environ, "OPENPORT_CONTRACT_OUTPUT": str(tmp_path)},
                            capture_output=True, text=True, timeout=120)
    assert result.returncode == 0, result.stdout + result.stderr
    files = list(tmp_path.glob("*.jsonl"))
    assert len(files) == 3, "Rebuild C++ tests with the contract fixtures"
    contract = Contract("", None, SPEC)
    for file in files:
        for line in file.read_text().splitlines():
            row = json.loads(line)
            contract.validate(**row)
    assert len(contract.checked) >= 30


def test_cli_token_validation_never_starts_a_provider_or_logs_secrets(tmp_path):
    import os
    import subprocess
    from conftest import ROOT
    binary = ROOT / "build/apps/openportd"
    if not binary.exists():
        pytest.skip("Build openportd to check argument validation")
    env = {key: value for key, value in os.environ.items() if key != "OPENPORT_WRITE_TOKEN"}
    result = subprocess.run([str(binary), "--require-token"], env=env, capture_output=True, text=True, timeout=5)
    assert result.returncode != 0
    assert "--require-token needs a configured token" in result.stderr
    token_file = tmp_path / "tokens"
    token_file.write_text("agent invalid do-not-print-this-secret\n")
    result = subprocess.run([str(binary), "--token-file", str(token_file)], env=env, capture_output=True, text=True, timeout=5)
    assert result.returncode != 0
    assert "Invalid token file at line 1" in result.stderr
    assert "do-not-print-this-secret" not in result.stderr
    token_file.write_text("agent read,trade:main,replay do-not-print-this-secret\n")
    result = subprocess.run([str(binary), "--token-file", str(token_file), "--require-token", "--help"], env=env, capture_output=True, text=True, timeout=5)
    assert "usage: openportd" in result.stderr
    assert "unknown option" not in result.stderr
    assert "do-not-print-this-secret" not in result.stderr


def test_archived_run_without_metadata_is_described(stub):
    listing = stub.respond("GET", "/api/replay", {}, None)[1]
    listing["history"] = [{"id": "old", "file": "old", "demo": False, "result": "open", "pnl": None,
                           "finished": True, "read_only": True, "error": "synthetic damaged journal"}]
    Contract("", None, SPEC).validate("GET", "/api/replay", 200, listing)
