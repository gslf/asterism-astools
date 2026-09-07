# Reviewed MCP stdio packages

⁂ astools can call a locally packaged MCP server through `runtime.protocol: "mcp"`
and `runtime.mode: "persistent"`. This optional adapter shares the existing
package checks, admission queues, host grants, sandbox and process supervisor.
It requires MCP **2026-07-28**. It probes `server/discover`, then includes version,
client identity and empty client capabilities on every request. There is no
initialization handshake, implicit downgrade, or server-initiated JSON-RPC reply.
See the protocol's [versioning](https://modelcontextprotocol.io/specification/2026-07-28/basic/versioning)
and [stdio binding](https://modelcontextprotocol.io/specification/2026-07-28/basic/transports/stdio).

## Package and authority

The operator supplies the executable and a reviewed JSON contract. Run:

```sh
python3 scripts/package_mcp.py --binary /absolute/path/to/server \
  --contract /absolute/path/to/reviewed.json --output /new/registry/my-server
build/astools-check /new/registry/my-server
```

Assembly copies the executable and records its SHA-256; it does not execute,
discover, register, enable, or download the server. The destination must be new.
Shared libraries and interpreter dependencies are not bundled or identified by
that binary hash. Register and approve the resulting package using the ordinary
host configuration and lockfile workflow. Linux and macOS package assembly is
supported; only Linux execution has been tested for this adapter.

The contract has exactly `id`, `version`, `summary`, `description`, `arguments`,
`permissions`, and `commands`. `arguments` is a literal argv array (at most 64
strings). Commands use the ordinary ⁂ astools parameter, path and permission types,
plus an explicit binding:

```json
{
  "name": "echo",
  "summary": "Echo one message",
  "params": [{"name": "msg", "required": true, "type": {"kind": "string"}}],
  "mcp": {
    "name": "remote.echo",
    "input_schema": {
      "type": "object",
      "properties": {"msg": {"type": "string"}},
      "required": ["msg"],
      "additionalProperties": false
    }
  }
}
```

The assembler encodes the schema objects as JSON strings in `manifest.xcdn`.
`output_schema` is optional. Each captured schema is limited to 64 KiB; the
reviewed contract and assembled manifest are each limited to 1 MiB. Full schema
admission happens in `astools-check` and when the runtime loads the manifest.

Only locally declared commands enter catalog, grammar, discovery and invocation.
Native JSON Schema export includes the intersection of local parameter types and
the captured MCP input schema. Text catalog/grammar represent local types; the
runtime additionally enforces all captured assertions. Remote descriptions,
instructions, icons, cache metadata and annotations never change permissions,
model-facing descriptions or cacheability. MCP commands cannot opt into ⁂ astools
answer caching. Before every invocation, bounded `tools/list` pagination checks
that the chosen remote name and both schemas still match their reviewed copies.
Object ordering is ignored; array ordering and every keyword remain significant.
This check cannot attest the server's implementation or atomically bind its later
execution to the listed schema.

Credentials follow [MCP stdio authentication](https://modelcontextprotocol.io/specification/2026-07-28/basic):
the host environment is scrubbed and only effective `permissions.env` grants are
copied. The process works in its managed scratch directory; `ASTOOLS_WORKSPACE`
identifies the workspace. Use typed path parameters for host path authorization.
Package hashes, environment grants and permissions are host decisions, never
inferred from remote hints. No HTTP transport or OAuth flow is implemented here.

## Supported schema profile

This is a **restricted profile**, not a claim of full MCP/JSON Schema conformance.
Unspecified dialect means 2020-12; the only explicit dialect accepted is
`https://json-schema.org/draft/2020-12/schema`. Supported assertions are:

- Boolean schemas; one primitive `type`; `const` and nonempty unique `enum`.
- Objects: `properties`, `required`, `additionalProperties`, `minProperties`, `maxProperties`.
- Arrays: one `items` schema, `minItems`, `maxItems`.
- Strings: `minLength`, `maxLength`, counted in Unicode code points.
- Numbers: `minimum`, `maximum`, `exclusiveMinimum`, `exclusiveMaximum`.

Annotations `title`, `description`, `$comment`, `default`, `examples`, `readOnly`,
`writeOnly` and `deprecated` are checked for their declared shapes. Unknown
keywords, references, composition, patterns, tuple items, multiple types, other
dialects and unresolved external schemas are rejected; none are silently stripped
or fetched. Numeric schema bounds stay within ±2^53 for portable comparisons;
integer instance comparisons preserve their int64 values. Schema traversal allows
32 levels and 1,024 subschemas; property/enum/required arrays allow at most 256
entries. Validation allows 32,768 visited instance nodes and 64 levels.

Input must declare an object root. Output can be any supported schema, including
arrays, scalar types and boolean schemas. Arguments are validated in the shared public/selection preflight, before process
startup or `tools/call`. MCP preflight also applies host path policy before
checking the captured schema, so batch admission sees the canonical arguments. A successful result with an output schema must carry matching
`structuredContent`; provided structured content is checked even on tool errors.
Dates, tags and other non-JSON xCDN extensions cannot pass through a lossy encoder.
Broader interoperability needs a separately measured, complete 2020-12 validator.

## Results and lifecycle

`result_xcdn` retains the complete JSON result, including error or incomplete
payloads when `ok` is false. `isError: true` becomes `astools/mcp-tool`, not a
transport success claim. A state-only `input_required` result is retained as
`astools/mcp-input-required`; the runtime does not inspect the state or retry the
operation. Sampling, roots and elicitation requests are rejected because no such
capabilities were advertised. No result is promoted to a runtime verification
receipt. Resource links, embedded resources, images and audio remain data; the
client does not open, render or fetch them. See [tool results](https://modelcontextprotocol.io/specification/2026-07-28/server/tools).

Each message is capped at 1 MiB and the configured output limit still applies.
Discovery permits 16 pages and 1,024 tools, cursor length 512 bytes; each exchange
permits 256 notifications and each tool result 128 content blocks. Duplicate
JSON fields, wrong response IDs, changed bindings and malformed envelopes fail
the exchange. Cancellation sends a best-effort notification (at most 50 ms) then
retires the process; deadline, malformed output and uncertain calls are never
automatically replayed. Graceful close sends EOF before the ordinary bounded
process-group cleanup. Stderr is drained during exchanges, not exposed as an
interactive stream. The process lifetime belongs to the host context/package,
not a conversation. See [persistent runtime](persistent-runtime.md) for platform,
descendant and dependency limits.

`test_mcp_contract`, `test_mcp_client` and `test_mcp_package` cover schema precision,
Unicode, bounded validation, current per-request metadata, pagination, immutable
bindings, denied client capabilities, full failure payloads, incomplete results,
output schemas, cancellation/deadlines, process reuse and local package assembly.
The package test crosses both stdio hops of the actual `astools-mcp` frontend and
client, including Linux basic/strict isolation and synthetic environment grants.
These are deterministic contract tests with a scripted C server; third-party
server interoperability, HTTP/OAuth, subscriptions, full MRTR and Windows/macOS
execution remain separate gates.
