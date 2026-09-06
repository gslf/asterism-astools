#!/usr/bin/env python3
"""Assemble an operator-reviewed MCP stdio package; never discover or run a server."""
import argparse
import json
from pathlib import Path
from package_local import assemble as assemble_local


def unique(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate contract field: " + key)
        result[key] = value
    return result


def invalid_constant(value):
    raise ValueError("non-finite contract number: " + value)


def contract(path):
    with path.open("rb") as stream:
        data = stream.read(1024 * 1024 + 1)
    if len(data) > 1024 * 1024:
        raise ValueError("Contract exceeds 1 MiB")
    value = json.loads(data, object_pairs_hook=unique, parse_constant=invalid_constant)
    required = {"id", "version", "summary", "description", "permissions", "commands", "arguments"}
    if not isinstance(value, dict) or value.keys() != required:
        raise ValueError("Contract requires exactly: " + ", ".join(sorted(required)))
    args = value["arguments"]
    if (not isinstance(args, list) or len(args) > 64 or
            any(not isinstance(arg, str) or "\0" in arg for arg in args)):
        raise ValueError("Server arguments must be a literal argv array (at most 64 strings)")
    if not isinstance(value["commands"], list) or not 1 <= len(value["commands"]) <= 256:
        raise ValueError("Declare 1–256 locally allowed commands")
    for command in value["commands"]:
        binding = command.get("mcp") if isinstance(command, dict) else None
        if not isinstance(binding, dict) or not {"name", "input_schema"} <= binding.keys():
            raise ValueError("Every command needs a reviewed MCP name and input schema")
        for key in ("input_schema", "output_schema"):
            if key not in binding:
                continue
            schema = binding[key]
            if (not isinstance(schema, (dict, bool)) or
                    (key == "input_schema" and (not isinstance(schema, dict) or schema.get("type") != "object"))):
                raise ValueError("MCP input must be an object schema; output accepts a schema object or boolean")
            binding[key] = json.dumps(schema, ensure_ascii=False, separators=(",", ":"), allow_nan=False)
            if len(binding[key].encode("utf-8")) > 65536:
                raise ValueError("Each reviewed schema is limited to 64 KiB")
    return value


def assemble(binary, reviewed, output):
    value = contract(reviewed)

    def manifest(os_name, arch):
        return "#astools_tool " + json.dumps({"manifest_version": 1, "kind": "executable",
            "id": value["id"], "title": value["id"], "version": value["version"],
            "summary": value["summary"], "description": value["description"],
            "platforms": [os_name], "permissions": value["permissions"], "commands": value["commands"],
            "runtime": {"mode": "persistent", "protocol": "mcp", "parallel": 1,
                "entry": [{"os": os_name, "arch": arch, "argv": ["bin/server", *value["arguments"]]}]}},
            ensure_ascii=False, separators=(",", ":"), allow_nan=False)

    return assemble_local(binary, output, "server", manifest)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--contract", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        print(assemble(args.binary, args.contract, args.output))
    except (OSError, ValueError, RecursionError) as error:
        parser.exit(1, f"package_mcp: {error}\n")


if __name__ == "__main__":
    main()
