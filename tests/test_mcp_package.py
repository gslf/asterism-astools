"""Exercise reviewed local packages through the real MCP host and stdio client."""
import argparse
import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from package_mcp import assemble


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--checker", required=True)
    parser.add_argument("--mcp", required=True)
    parser.add_argument("--server", required=True, type=Path)
    args = parser.parse_args()
    reviewed = {"id": "remote", "version": "1.0.0", "summary": "Reviewed local echo",
        "description": "A local fixture for the MCP client contract.", "arguments": ["normal"],
        "permissions": {"fs": [{"path": "${workspace}", "access": "read-write"}], "net": False, "proc": False, "env": []},
        "commands": [{"name": "echo", "summary": "Echo one message", "annotations": {"read_only": False},
            "params": [{"name": "msg", "required": True, "type": {"kind": "string"}}],
            "mcp": {"name": "remote.echo", "input_schema": {"type": "object", "properties": {"msg": {"type": "string"}},
                "required": ["msg"], "additionalProperties": False}}}]}
    with tempfile.TemporaryDirectory(prefix="astools-mcp-package-") as directory:
        root = Path(directory)
        for level, mode in (("basic", "normal"), ("strict", "normal"), ("basic", "environment"), ("strict", "environment"), ("basic", "environment-denied"), ("basic", "failed"), ("basic", "schema")):
            work = root / (level + "-" + mode); work.mkdir()
            contract = work / "contract.json"
            spec = copy.deepcopy(reviewed); spec["arguments"] = [mode]
            if mode.startswith("environment"):
                spec["permissions"]["env"] = ["MCP_TEST_ALLOWED"]
            contract.write_text(json.dumps(spec), encoding="utf-8")
            package = assemble(args.server, contract, work / "registry" / "remote")
            assert not (work / "called").exists(), "Assembly executed the server"
            subprocess.run([args.checker, str(package)], check=True, capture_output=True, timeout=10)
            try:
                assemble(args.server, contract, package)
            except FileExistsError:
                pass
            else:
                raise AssertionError("Existing package was overwritten")
            config = work / "config.xcdn"
            config.write_text("#astools_config " + json.dumps({
                "registry": {"paths": [{"path": str(package.parent), "trust": "standard"}], "watch": "off", "pinning": "off"},
                "grants": {"tools": [{"tool": "remote", "env": ["MCP_TEST_ALLOWED"]}] if mode == "environment" else []},
                "workspace": {"root": str(work)}, "sandbox": {"default_level": level, "strict_fallback": "reject"}}))
            meta = {"io.modelcontextprotocol/protocolVersion": "2026-07-28",
                    "io.modelcontextprotocol/clientCapabilities": {}}
            requests = [{"id": 1, "method": "server/discover", "params": {"_meta": meta}},
                {"id": 2, "method": "tools/list", "params": {"_meta": meta}},
                {"id": 3, "method": "tools/call", "params": {"_meta": meta, "name": "remote_echo", "arguments": {"msg": "tè 🍵"}}}]
            payload = "".join(json.dumps({"jsonrpc": "2.0", **item}) + "\n" for item in requests)
            run = subprocess.run([args.mcp, "--config", str(config)], input=payload, text=True,
                                 capture_output=True, timeout=30, env={**os.environ,
                                     "MCP_TEST_ALLOWED": "synthetic-test-credential",
                                     "MCP_TEST_DENIED": "must-not-reach-child"})
            assert run.returncode == 0, run.stderr
            messages = {row["id"]: row for row in map(json.loads, run.stdout.splitlines()) if "id" in row}
            advertised = messages[2]["result"]["tools"]
            assert len(advertised) == 1 and advertised[0]["name"] == "remote_echo", advertised
            assert not advertised[0]["annotations"]["readOnlyHint"], advertised
            if mode == "schema":
                assert "error" in messages[3] and not (work / "called").exists(), messages[3]
            else:
                assert "result" in messages[3], (level, mode, messages[3], run.stderr)
                response = messages[3]["result"]
                assert response["isError"] == (mode == "failed"), response
                nested = json.loads(response["content"][0]["text"])
                assert nested["structuredContent"]["arguments"]["msg"] == "tè 🍵", nested
                assert nested["content"][0]["text"] == "tè 🍵", nested
        negative = root / "negative.json"
        negative.write_text('{"id":"one","id":"two"}')
        try:
            assemble(args.server, negative, root / "invalid")
        except ValueError:
            pass
        else:
            raise AssertionError("Duplicate contract fields were accepted")
        assert not (root / "invalid").exists()
    print("MCP package: basic/strict invocation, reviewed discovery, failed-result retention and schema rejection passed")


if __name__ == "__main__":
    main()
