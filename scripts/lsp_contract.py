"""The closed clangd tool contract, emitted as ordinary tagged xCDN."""
import json


class Tagged(dict):
    def __init__(self, tag, **fields):
        super().__init__(fields)
        self.tag = tag


def render(value, depth=0):
    if isinstance(value, dict):
        prefix = f"#{value.tag} " if isinstance(value, Tagged) else ""
        body = ",\n".join("  " * (depth + 1) + json.dumps(k) + ": " + render(v, depth + 1)
                          for k, v in value.items())
        return prefix + "{\n" + body + "\n" + "  " * depth + "}"
    if isinstance(value, list):
        return "[" + ", ".join(render(v, depth) for v in value) + "]"
    return json.dumps(value, ensure_ascii=False)


def param(name, kind, description, required=True, **constraints):
    return Tagged("param", name=name, type=Tagged("type", kind=kind, **constraints),
                  required=required, description=description)


def manifest(os_name, arch):
    span = [param(k, "integer", "UTF-8 byte offset or one-based line/byte column.")
            for k in ("start_byte", "end_byte", "start_line", "start_column", "end_line", "end_column")]
    fields = [param("path", "string", "Workspace-relative source."),
              param("sha256", "string", "Hash of bytes read by the host; server dependency freshness is not certified."),
              param("range", "object", "Half-open range.", fields=span),
              param("selection", "object", "Selected name, when supplied.", False, fields=span)]
    for key, kind in (("name", "string"), ("kind", "integer"), ("message", "string"), ("severity", "integer")):
        fields.append(param(key, kind, "Optional LSP symbol or diagnostic field.", False))
    results = Tagged("type", kind="object", fields=[
        param("provider", "string", "Negotiated server name and version."),
        param("position_encoding", "string", "Always utf-8."),
        param("source_sha256", "string", "Version of the document sent to didOpen."),
        param("coverage", "string", "Limits of this observation; never a verification receipt."),
        param("truncated", "boolean", "True when the result limit excluded rows."),
        param("excluded", "integer", "Locations outside authorized readable workspace files."),
        param("items", "array", "Up to 128 normalized observations.", item=Tagged("type", kind="object", fields=fields)),
    ])
    commands = []
    for name in ("symbols", "definition", "references", "diagnostics"):
        params = [param("path", "path", "Observed C/C++ file.", access="read", must_exist=True),
                  param("sha256", "string", "Entire-file SHA-256 from code.read-range; stale versions fail.", min_len=64, max_len=64)]
        if name in ("definition", "references"):
            params += [param(k, "integer", "One-based UTF-8 line or byte column.", min=1, max=2147483647)
                       for k in ("line", "column")]
        commands.append(Tagged("command", name=name, summary=f"Query C/C++ {name} with clangd.",
            description="Read-only server observations, limited by the configured compilation database and index. "
                        "Headers, toolchain and target-file freshness are not certified. Empty diagnostics do not prove success.",
            annotations={"read_only": True}, params=params,
            returns={"type": results, "description": "Current host file hashes accompany server locations."}))
    return Tagged("astools_tool", manifest_version=1, id="clangd", version="1.0.0",
        title="C/C++ semantic navigation", summary="Optional clangd symbols, definitions, references and diagnostics.",
        description="Managed LSP process with UTF-8 positions and exact open-document preconditions.",
        kind="executable", platforms=[os_name], runtime={"mode": "persistent", "protocol": "lsp", "parallel": 1,
            "entry": [{"os": os_name, "arch": arch, "argv": ["bin/clangd", "--background-index=false",
                "--enable-config=false", "--clang-tidy=false", "--pch-storage=memory", "--log=error", "-j=2"]}]},
        permissions={"fs": [{"path": "${workspace}", "access": "read"}], "net": False, "proc": False, "env": []},
        commands=commands)
