# ⁂ astools

⁂ Asterism Tools, a local, plug-and-play system-tool registry for LLM agents.

A *tool* is a directory dropped into a registry root. One xCDN manifest inside it is simultaneously the machine contract (commands, typed parameters, constraints, permissions) and the conversational interface (descriptions and worked examples the model reads).


## What ships

| Artifact | Role |
|---|---|
| `libastools` | C99 library: registry, manifest/type engine, argument validation, policy, sandboxed invocation, catalog / GBNF / JSON Schema generation |
| `astools-mcp` | MCP server over stdio exposing every registered tool |
| `astools-std` | The standard suite — semantic `code` / `project` tools plus `fs`, `grep`, `edit`, `git`, `proc`, `sys`, `env` (+ optional `net`) — packaged like any third-party tool |
| `astools-check` | Manifest validator/linter, catalog preview, lockfile approval, plugin lint |
| `astools-jail` | Internal sandbox helper (Seatbelt on macOS) |
| `astools-plugin` | Agent Plugins 1.0.0 package pointing any conformant client at `astools-mcp` |

## Build

```bash
git submodule update --init
cmake -B build
cmake --build build -j
ctest --test-dir build
```

Requires CMake ≥ 3.16 and a C99 toolchain. The only dependency is the [xCDN-C](https://github.com/gslf/xCDN-C) submodule.

Options: `ASTOOLS_BUILD_MCP` / `ASTOOLS_BUILD_STD` / `ASTOOLS_BUILD_CHECK` / `ASTOOLS_BUILD_TESTS` / `ASTOOLS_BUILD_PLUGIN` (ON), `ASTOOLS_BUILD_NET` / `ASTOOLS_NO_THREADS` / `ASTOOLS_SANITIZERS` (OFF).

## Quick start

```bash
# a workspace with the standard tools
mkdir -p /tmp/ws && cd /tmp/ws
cp -R <build>/packages tools

# validate a manifest, preview what the model will see
astools-check tools/fs
astools-check --catalog --root tools --workspace .

# serve everything over MCP
astools-mcp --root tools --workspace .
```

From C:

```c
astools_open_params p = { .workspace_root = "." };
astools_ctx *c;
astools_open(&p, &c);

char *catalog;
astools_catalog(c, ASTOOLS_CATALOG_FULL, 8000, &catalog);
/* inject into the system prompt; the model answers with
 *   CALL fs.read {path: "notes/todo.txt"}                */

astools_result r;
astools_invoke(c, "fs", "read", "{path: \"notes/todo.txt\"}", 0, &r);
```

## Safety model

Deny by default: a manifest *requests* permissions, only the host *grants* them, and the effective set is the intersection. Every executable tool runs as a child process with a scrubbed environment, a private scratch cwd, a wall-clock deadline, and output caps; `path`-typed arguments are canonicalized and checked against the grants before anything spawns. The `strict` sandbox level adds kernel enforcement where the platform provides it (Seatbelt on macOS), and `astools_get_sandbox_caps` reports honestly what is actually enforced.

The scrubbed `PATH` contains only fixed system directories by default. Add
operator-approved compiler and build-system directories with
`sandbox.executable_paths`, for example
`["C:/llvm/bin", "C:/Program Files/CMake/bin"]` on Windows. These entries are
visible to both `proc.run` and the closed-argv `project.*` workflows without
implicitly inheriting the host's complete `PATH`.

For coding agents, prefer `project.build` / `test` / `lint` / `format` /
`diagnostics` and `code.read-range` / `search-symbol` / `apply-patch`.
Their schemas contain no executable or argv field. The `project` proc grant is
scoped to that tool and does not enable the arbitrary `proc.run` escape hatch.


## License
MIT [LICENSE](LICENSE).
