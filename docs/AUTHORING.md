# Authoring an astools tool

A tool is a directory. Everything the runtime and the model need to know about it lives in one file, `manifest.xcdn`. This guide walks through building a tool from scratch. The `docs/SPEC.md` is the binding reference and
`packages/fs/manifest.xcdn` is a complete worked example.

## 1. Layout

```
mytool/                        # directory name = tool id (advisory)
├── manifest.xcdn              # the contract — required
├── bin/
│   ├── linux-x86_64/mytool
│   ├── macos-arm64/mytool
│   └── windows-x86_64/mytool.exe
└── docs/                      # optional, ignored by the runtime
```

Install = copy the directory into a registry root. Remove = delete it.

## 2. The protocol (oneshot)

Your executable is started once per invocation. It reads exactly one `#tool_request` from stdin, does the work, writes exactly one
`#tool_response` to stdout, and exits. stderr is yours for logging (the runtime captures a bounded amount of it).

```xcdn
#tool_request {
  protocol: 1,
  invocation_id: u"0d9f0c9e-…",
  tool: "mytool", version: "1.0.0", command: "run",
  args: { … },                 // validated, defaults injected,
                               // paths canonical + absolute
  deadline: t"2026-08-02T10:15:32Z",
  limits: { max_output_bytes: 1048576 },
  grants: { fs: […], net: false, proc: false, env: [] },
  workspace: "/abs/workspace",
  scratch:  "/abs/workspace/.astools/tmp/<invocation_id>",
}
```

Reply with either:

```xcdn
#tool_response { protocol: 1, invocation_id: u"…", ok: true,
                 result: { … } }
#tool_response { protocol: 1, invocation_id: u"…", ok: false,
                 error: { code: "mytool/bad-input", message: "…",
                          retriable: false } }
```

Rules that keep you honest:

- **Trust the args.** The runtime already type-checked them, injected the defaults, and canonicalized every `path`-typed value to an absolute path inside the granted prefixes. Never re-interpret relative paths.
- **stdout is sacred.** Only the response goes there. Diagnostics → stderr.
- **Respect the deadline.** At the deadline your whole process group is killed; a graceful early error beats a corpse.
- **Stay under `limits.max_output_bytes`.** An oversized response is treated as a malfunction (`astools/overflow`), not truncated.
- Your cwd is a private scratch directory; it disappears after the call.

## 3. Writing the manifest

Start from this skeleton and grow it:

```xcdn
#astools_tool {
  manifest_version: 1,
  id: "mytool",                 // [a-z0-9][a-z0-9-]{0,31} — no underscore
  version: "1.0.0",             // SemVer 2.0.0
  title: "My tool",
  summary: "One line, max 80 chars — the index catalogs show only this.",
  description: """
    Model-facing prose: what the tool is for, when to use it, caveats.
  """,
  kind: "executable",
  platforms: ["linux", "macos", "windows"],
  runtime: {
    mode: "oneshot",
    entry: [
      { os: "macos", arch: "arm64", argv: ["bin/macos-arm64/mytool"] },
      // one entry per supported (os, arch)
    ],
  },
  permissions: {
    fs: [ { path: "${workspace}", access: "read-write" } ],
    net: false, proc: false, env: [],
  },
  commands: [ /* … */ ],
}
```

Per command, invest in three things — they are what the model actually
reads:

1. **`summary`** — one sharp line.
2. **`examples`** — at least one `#example { call, result }` with realistic values. Catalogs render the first one as a `CALL` line; it teaches the syntax by showing it.
3. **Types.** Type filesystem inputs as `path` (never `string`), that is the hook that lets the policy engine check them. Use `enum` instead of documenting magic strings. Constrain sizes (`max_bytes`, `max_items`): the validator enforces them for free.

Annotations (`read_only`, `destructive`, `idempotent`, `long_running`) drive catalog markers and MCP hints; set them truthfully. Declare your error codes (`mytool/<slug>`) so they render into the docs.

## 4. Permissions are requests

The manifest asks; only the host grants. Request the narrowest set that works,`${workspace}` read-write is the norm for file tools, `proc` and `net` are deny-by-default and should be requested only when the tool's whole point is spawning programs or reaching the network. If your tool runs without a grant it requested, fail early with a clear tool error. The runtime already denies invocations that require an ungranted `proc` or `net`.

## 5. Check, approve, iterate

```bash
astools-check mytool/                 # schema + lint
astools-check --catalog --root <root> # what the model will see
astools-check --approve mytool --root <root>   # record lockfile hashes
```

Versioning discipline: within a major version, only add new commands, new optional params, widened constraints. Removing/renaming anything, adding a required param, or narrowing a type is also a major bump. Mark commands `deprecated: true` for at least one minor release first.
