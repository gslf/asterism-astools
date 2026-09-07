# Optional semantic navigation with clangd

⁂ astools can manage an LSP 3.17 process through the same admission queues,
package validation, sandbox and cancellation paths as persistent tools. The first
profile supports C/C++ with clangd. It is optional: `code.search-symbol` remains a
bounded identifier text search, and `code.read-range` works without a server.

The adapter exposes four fixed commands. Each requires `path` and an entire-file
`sha256`, obtained from a prior `code.read-range` result. `definition` and
`references` also require one-based `line` and `column`; columns count UTF-8 bytes.
A byte inside a multibyte character is invalid. Files must be regular UTF-8 text,
without NUL, no larger than 1 MiB.

| Command | Observation |
| --- | --- |
| `clangd.symbols` | Document symbols, with nested children flattened |
| `clangd.definition` | Locations or location links for the selected token |
| `clangd.references` | Server references, including declarations |
| `clangd.diagnostics` | Published diagnostics for the exact opened document version |

MCP names use underscores, for example `clangd_definition`. Ordinary ⁂ astools
invocation uses tool `clangd`, command `definition`.

## Assemble and enable locally

Python 3.11+ assembles a package from an operator-selected executable. The command
does not download, install globally, grant permissions or overwrite a package:

```sh
python3 scripts/package_clangd.py --binary /usr/bin/clangd --output /tmp/my-registry/clangd
build/astools-check /tmp/my-registry/clangd
```

After successful assembly, add `/tmp/my-registry` to the host's registry paths at
`standard` trust and approve its content through the normal lockfile workflow.
The executable is copied inside the package, so entry-byte hashing covers it.
`provenance.json` records the copied SHA-256 and platform. Shared libraries,
resource headers, compilation databases and toolchains remain external; this is
not a portable or signed LLVM distribution. Redistributing LLVM binaries also
requires the applicable upstream license material.

The manifest requests workspace reads only, with networking and child-process
capabilities disabled. It uses fixed arguments: repository `.clangd` configuration,
background indexing and clang-tidy are disabled; preambles stay in memory; worker
threads are limited to two. No `--query-driver` execution whitelist is installed.
Initialization explicitly selects the workspace as the compilation database
directory, avoiding ancestor-directory discovery. Missing headers or an incomplete
build environment can still make the analysis incomplete.

Choose strict sandboxing with `strict_fallback: "reject"` when filesystem and
network confinement are required. Basic mode performs host checks but does not
confine all reads performed internally by clangd. System dependency reads depend
on the selected OS sandbox profile. The current reader supports POSIX; Windows
LSP invocation returns unsupported before spawning. Linux clangd 22.1.8 was tested
in basic and available strict modes. Other OS/backend combinations are not
certified by those runs.

## Version and trust boundaries

The server must explicitly negotiate `positionEncoding: "utf-8"` and open/close
synchronization. Provider names alone confer no capabilities. Before each query,
the host reads authorized source bytes without following symlinks, including in
workspace-root ancestors, and compares the required SHA-256. It sends those exact
bytes with `didOpen`. Only diagnostics with the matching URI **and version** can
complete the diagnostics request; stale/versionless notifications cannot turn
into an empty successful response.

Locations are limited to authorized files within the workspace. The host validates
ranges, re-reads referenced files, and refuses changed hashes before returning.
The result includes host-observed file hashes, half-open byte ranges, one-based
line/byte columns, the provider identity, exclusions and truncation. There are at
most 128 results, 16 distinct files and 32 symbol nesting levels. An oversized
frame, excessive notification stream or malformed result fails explicitly.

These hashes identify bytes read by the host. They do **not** certify that the
server parsed that exact revision of every target file, header or dependency.
The server's compilation database, preamble and index may be incomplete or stale.
The `coverage` field states this limitation. Reads and the final recheck are not
an atomic workspace snapshot or a filesystem compare-and-swap operation.
Diagnostics, including an empty list, are observations; they never create a
verification receipt or certify that a patch passed its acceptance tests.

All server-initiated requests receive a refusal, including workspace edits and
command execution. Descriptions, diagnostics and symbol names are untrusted data.
The host's fixed adapter contract cannot be expanded by server metadata. LSP
commands cannot opt into the manifest's idempotent annotation; their results
must not be cached using only the source hash while dependencies can change.

## Lifecycle and validation

`runtime.protocol: "lsp"` is valid only for executable persistent tools. Omitting
it selects the existing ⁂ astools protocol. The manifest parser requires the closed
command names, read-only annotations and exact required argument types. Discovery,
JSON Schema export and invocation use that same manifest.

One borrower owns the process stream for an entire query. Length-delimited UTF-8
JSON-RPC shares the bounded pipe implementation; duplicate keys, malformed frames,
wrong IDs and incompatible capabilities are rejected. Server stderr is drained,
not exposed as a separate user stream. Successful requests close their document.
Failed, timed-out or cancelled exchanges kill the managed process and clear its
pending messages; subsequent requests start a fresh process. There is no automatic
replay of the interrupted query. Normal idle reclamation and package replacement
use the persistent runtime's lifecycle. This is not a general LSP editor client,
AST index, durable process API or complete repository graph.

CTest includes hostile peer tests, URI/range/hash/permission regressions and,
when clangd is installed, actual overload, Unicode, reference and diagnostic
queries. The package smoke test assembles the real binary, rejects invalid
contracts, checks repository config suppression, exports MCP discovery and invokes
the packaged server with its declared output schema. Missing optional test
dependencies are reported at configure time; simulated peers are not semantic
accuracy measurements.

Protocol sources: [LSP 3.17 specification](https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/),
[clangd initialization extensions](https://clangd.llvm.org/extensions),
[clangd configuration](https://clangd.llvm.org/config).
