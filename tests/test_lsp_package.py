"""Validate the generated contract and query a packaged binary via the MCP frontend."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from lsp_contract import manifest, render
from package_clangd import assemble


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--checker", required=True)
    parser.add_argument("--mcp", required=True)
    parser.add_argument("--clangd", required=True, type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="astools-lsp-package-") as directory:
        root = Path(directory)
        package = assemble(args.clangd, root / "registry" / "clangd")
        subprocess.run([args.checker, str(package)], check=True, timeout=10)
        original = (package / "bin" / "clangd").stat().st_size
        try:
            assemble(args.clangd, package)
        except FileExistsError:
            pass
        else:
            raise AssertionError("Existing package was replaced")
        assert (package / "bin" / "clangd").stat().st_size == original
        contract = manifest("linux", "x86_64")
        for case in ("protocol", "oneshot", "command", "path", "hash", "effect", "cache"):
            bad = copy.deepcopy(contract)
            cmd = bad["commands"][0]
            if case == "protocol": bad["runtime"]["protocol"] = "unknown"
            elif case == "oneshot": bad["runtime"]["mode"] = "oneshot"
            elif case == "command": cmd["name"] = "execute"
            elif case == "path": cmd["params"][0]["type"]["kind"] = "string"
            elif case == "hash": cmd["params"][1]["required"] = False
            elif case == "effect": cmd["annotations"]["read_only"] = False
            elif case == "cache": cmd["annotations"]["idempotent"] = True
            negative = root / "negative.xcdn"
            negative.write_text(render(bad), encoding="utf-8")
            result = subprocess.run([args.checker, str(negative)], capture_output=True, timeout=10)
            assert result.returncode == 2, (case, result.stdout, result.stderr)
        workspace = root / "workspace"
        workspace.mkdir()
        source = "#ifndef ASTOOLS_HIDE_SYMBOL\nint identity(int x) { return x; }\n#endif\n"
        (workspace / ".clangd").write_text("CompileFlags:\n  Add: [-DASTOOLS_HIDE_SYMBOL]\n", encoding="utf-8")
        (workspace / "example.cpp").write_text(source, encoding="utf-8")
        config = root / "config.xcdn"
        config.write_text('#astools_config ' + json.dumps({
            "registry": {"paths": [{"path": str(package.parent), "trust": "standard"}], "watch": "off", "pinning": "off"},
            "workspace": {"root": str(workspace)},
            "sandbox": {"default_level": "basic", "strict_fallback": "reject"},
        }), encoding="utf-8")
        requests = [
            {"id": 1, "method": "initialize", "params": {"protocolVersion": "2025-06-18"}},
            {"method": "notifications/initialized"},
            {"id": 2, "method": "tools/list"},
            {"id": 3, "method": "tools/call", "params": {"name": "clangd_symbols", "arguments": {
                "path": "example.cpp", "sha256": hashlib.sha256(source.encode()).hexdigest()}}},
        ]
        requests.append({"id": 4, "method": "tools/call", "params": {"name": "clangd_symbols", "arguments": {
            "path": "example.cpp", "sha256": "0" * 64}}})
        payload = "".join(json.dumps({"jsonrpc": "2.0", **item}) + "\n" for item in requests)
        run = subprocess.run([args.mcp, "--config", str(config)], input=payload, text=True, capture_output=True, timeout=30)
        assert run.returncode == 0, run.stderr
        messages = {item["id"]: item for item in map(json.loads, run.stdout.splitlines()) if "id" in item}
        assert {"clangd_symbols", "clangd_definition", "clangd_references", "clangd_diagnostics"} <= {
            tool["name"] for tool in messages[2]["result"]["tools"]}, messages
        result = messages[3]
        assert "error" not in result and not result["result"]["isError"], (result, run.stderr)
        observation = json.loads(result["result"]["content"][0]["text"])
        assert observation["items"][0]["name"].startswith("identity"), observation
        assert observation["source_sha256"] == hashlib.sha256(source.encode()).hexdigest()
        assert "verification" not in observation
        assert "reopen the file" in messages[4]["error"]["message"], messages[4]
        print("Packaged clangd: schema, negative contracts, discovery and MCP query passed")


if __name__ == "__main__":
    main()
