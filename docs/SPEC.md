# astools — Asterism Tools, Software Specification v0.3 (condensed)

> Condensed, normative transcription of the v0.1 spec + Amendment A1 (grep/edit/git) + Amendment A2 (project rename to astools, Agent Plugin). Naming: library `libastools`, header `astools.h`, prefix `astools_`, binaries `astools-mcp` / `astools-std` / `astools-check` / `astools-jail`, xCDN tags `#astools_tool` / `#astools_config` / `#astools_lock`, reserved error namespace `astools/…`, env vars `ASTOOLS_*`, lockfile `astools.lock.xcdn`, scratch `<workspace>/.astools/tmp/`. License MIT. C99 strict (`-Wall -Wextra -Wpedantic -Werror`).

## 1. What it is

A local, plug-and-play registry of system tools for LLM agents. A tool is a directory (package) dropped into a registry root; one xCDN manifest inside it is both the machine contract (types, constraints, permissions) and the conversational interface (descriptions, examples). No tool-specific code in the host/library/MCP server. Fully local; no network I/O in libastools (D9).

Design goals: G1 manifest = single source of truth; G2 small-model first (catalogs under char budgets, one-line CALL protocol, GBNF export); G3 safe by default (deny-by-default grants, child processes with limits+deadline, path canonicalization pre-flight, honest capability reporting); G4 minimal deps (xCDN-C submodule only; in-house JSON for MCP); G5 text-only state, no caches (D10); G6 Linux/macOS/Windows via OS shim; G7 a misbehaving tool can never crash/block/corrupt the host.

## 3. Tool model

### 3.1 Package
`<root>/<id>[@<version>]/` containing `manifest.xcdn` + `bin/<os>-<arch>/…`. Dir name advisory; manifest authoritative. Install = copy dir; remove = delete dir. Pinning-enforced stores also need lockfile approval.

### 3.2 Manifest `#astools_tool` fields
- `manifest_version` int, must be 1 (greater ⇒ reject package).
- `id` slug `[a-z0-9][a-z0-9-]{0,31}` (no underscore, D7).
- `version` SemVer 2.0.0 (MAJOR.MINOR.PATCH, optional pre-release; build metadata accepted, ignored for ordering).
- `title`, `summary` (≤80 chars, one line), `description` (prose, optional).
- `kind`: `"executable"` | `"library"`.
- `platforms`: subset of `linux|macos|windows`. No runtime entry for current platform ⇒ registered but unavailable (not callable, absent from catalog/MCP).
- `runtime`: `{ mode: "oneshot"(default)|"persistent", entry: [{os, arch, argv}], parallel(=1), idle_timeout(=PT2M), startup_timeout(=PT10S) }`. `argv[0]` resolved relative to package dir; absolute/PATH argv[0] only in "full"-trust roots (else reject package).
- `permissions`: `{ fs: [{path, access: "read"|"write"|"read-write"}], net: bool, proc: bool, env: [names] }` — requests, not rights; `${workspace}` expands at load time.
- `commands`: array of `#command`.
- `tags`,`license`,`authors` optional, engine-ignored.

### 3.3 `#command`
`name` (slug, unique in tool; FQN `<tool>.<name>`), `summary`, `description`, `annotations {read_only, destructive, idempotent, long_running}` (all default false), `timeout` (duration, overrides invocation.timeout), `params` (`#param` list), `returns {type, description}`, `errors [{code: "<tool>/<slug>", description}]`, `examples [#example {title?, call, result, note?}]` (≥1 per command else lint warning), `deprecated` bool (callable but flagged).

### 3.4 Type system (`#type`)
kinds: `string` (min_len,max_len) · `integer` (min,max) · `number` (min,max) · `boolean` · `bytes` b"…" (max_len over decoded bytes) · `datetime` t"…" RFC3339 · `duration` r"…" ISO8601 · `uuid` u"…" · `path` (string value; attrs `access: read|write|read-write`, `must_exist`) · `enum` (values:[strings]) · `array` (item #type, min_items, max_items) · `map` (value #type, max_entries, arbitrary string keys) · `object` (fields: closed #param list).

`#param`: name, type, required(=false), default (literal, optional params only), description, examples?.

Validation (before dispatch): strict, NO coercion ("3" ≠ integer, 1 ≠ true); unknown arg keys rejected (ERR_INVALID); missing required rejected; defaults injected for absent optionals (tools always see a complete canonical args object, D8); constraints enforced recursively; `path` params resolved and grant-checked after schema validation, before dispatch (§6.4). Filesystem inputs must be typed `path`, not `string` (lint error).

### 3.7 Versioning
Multiple versions may coexist (dirs `fs/`, `fs@2.0.0/`, several roots). Bare ref resolves to highest non-pre-release; `id@1.2.0` pins exactly; config `registry.pins` overrides bare refs. No ranges in v1. Within a major: append-only evolution.

## 4. Registry

### 4.1 Roots and trust
`registry.paths` ordered; env `ASTOOLS_PATH` (OS path-sep list) prepended. Trust: `standard` (default; only executable kind, package-relative entries) | `full` (also `library` kind, absolute/PATH entries). Same (id,version) in several roots: first root wins + warning. Different versions coexist.

### 4.2 Scan
Enumerate immediate subdirs of each root; parse manifest.xcdn; reject invalid packages with a warning (never fatal); select entry for current (os,arch) else unavailable; apply lockfile policy; build resolution map; publish atomically under write lock.

### 4.3 Refresh
At open, on explicit refresh, or (watch="poll") when a poll every poll_interval (default PT5S) sees changed mtimes on roots/package dirs. After a changed refresh, astools-mcp emits notifications/tools/list_changed. Running invocations keep their resolved version.

### 4.4 Lockfile `astools.lock.xcdn`
`#astools_lock { version: 1, tools: [{id, version, manifest_sha256: b"…", artifacts: [{path, sha256: b"…"}], approved_at: t"…"}] }`. `registry.pinning`: "off" | "warn" (default; log warning, stay enabled) | "enforce" (unknown/changed hashes ⇒ tool disabled until approved). Approval (`astools-check --approve <ref>` / `astools_tool_approve`) recomputes hashes. Atomic-replace writes. No derived caches anywhere (D10).

## 5. Invocation engine

### 5.1 Pipeline
1 resolve ref (reject unavailable/disabled/pin-blocked: ERR_NOT_FOUND / ERR_DENIED) → 2 parse+validate args, inject defaults (ERR_INVALID with precise param name) → 3 policy pre-flight (ERR_DENIED, nothing spawned) → 4 acquire slot (invocation.max_concurrent); deadline = arg ▷ command timeout ▷ invocation.timeout → 5 dispatch by kind/mode inside sandbox → 6 collect #tool_response enforcing deadline+caps → 7 result validation (result_validation off|warn|enforce; enforce ⇒ ERR_PROTOCOL on mismatch) → 8 counters + audit; release slot.

### 5.2 Oneshot (canonical, D1)
One process per invocation. Runtime writes exactly one `#tool_request` to child stdin and closes it; tool writes exactly one `#tool_response` to stdout and exits.
- Env: fully scrubbed, rebuilt as granted vars + minimal PATH (fixed system dirs) + HOME/TMPDIR=scratch + ASTOOLS_PROTOCOL=1, ASTOOLS_INVOCATION_ID, ASTOOLS_WORKSPACE, ASTOOLS_SCRATCH.
- cwd = fresh scratch `<workspace>/.astools/tmp/<invocation_id>`, deleted after collection (kept on failure at debug log level).
- stdout = protocol; stderr = free-form log, captured up to stderr_max_bytes, forwarded to log at debug (or error context).
- Exit code: well-formed response is authoritative regardless of exit code; nonzero exit with no response ⇒ ERR_TOOL, code "astools/tool-crashed" + stderr excerpt.

### 5.3 Persistent
Same stdio channel, xCDN streaming as framing. On start tool emits `#tool_hello {protocol, tool, version, parallel}` within startup_timeout (mismatch ⇒ kill + ERR_PROTOCOL). Requests matched by invocation_id; at most `parallel` in flight, others queue. Cancel: `#tool_cancel {invocation_id}`, grace PT2S, then kill+restart (caller sees ERR_TIMEOUT / ERR_CANCELLED). Crash ⇒ in-flight fail "astools/tool-crashed", lazy restart with backoff 1s→30s. Idle > idle_timeout ⇒ SIGTERM then SIGKILL after PT2S.

### 5.4 Library kind
dlopen from "full"-trust root, only when sandbox.allow_library. ABI: `astools_tool_entry()` returning vtable {abi_version==1, init(manifest), shutdown, invoke(request)->response, free_result}. Host privileges, no sandbox, advisory deadlines. Same wire types as §5.2.

### 5.5 Deadlines
Precedence: invoke arg ▷ command timeout ▷ invocation.timeout (default PT30S). Oneshot: kill whole process group/job at deadline. Persistent: cancel then kill-restart. Library: advisory.

### 5.6 Output limits
stdout cap invocation.max_output_bytes (default 1 MiB); exceeding ⇒ ERR_TOOL "astools/overflow" (kill, never truncate-and-continue). stderr cap stderr_max_bytes (default 256 KiB), oldest half discarded when full.

### 5.7 Errors
`#tool_response{ok, result | error{code, message, retriable}}`. Tool codes "<tool>/<slug>" (declared in manifest). Reserved runtime codes: astools/invalid-args, astools/denied, astools/timeout, astools/cancelled, astools/tool-crashed, astools/protocol, astools/overflow.

## 6. Sandboxing

Levels: `none` (debug) · `basic` (default; portable: process isolation, env scrub, scratch cwd, wall deadline, output caps, CPU/mem/nproc rlimits or Job Object; fs/net policy by pre-flight only) · `strict` (basic + kernel enforcement: Linux Landlock+seccomp+no_new_privs+empty netns; macOS Seatbelt profile via astools-jail; Windows restricted token; missing features ⇒ sandbox.strict_fallback "degrade" (default, warn) | "reject" (ERR_UNSUPPORTED)).

### 6.3 Grants
Effective = manifest request ∩ host grants. grants.workspace_access (default read-write) auto-grants the workspace subtree; grants.tools adds per-tool fs prefixes / net / proc / env. Missing grant ⇒ preflight ERR_DENIED with a message naming the missing grant. Effective set is serialized into #tool_request (informative; enforcement never relies on it).

### 6.4 Paths
path args: relative joined to workspace → canonicalize (realpath; for access "write" + !must_exist canonicalize deepest existing ancestor and re-append remainder) → must fall under ≥1 effective fs grant covering the declared access, else ERR_DENIED → canonical path REPLACES the original in the args object. TOCTOU honesty: basic = pre-flight only; strict = kernel authority.

## 7. Conversational interface

All renderings deterministic: identical registry ⇒ byte-identical output.

### 7.1 Catalog
Levels: index (one line per command: FQN, param names, annotation markers) · summary (default; + typed param lists + one-line summaries) · full (+ descriptions + one worked example per command rendered as a call line). Header explains the CALL syntax. Ordering: catalog.priority first, then id ascending; commands in manifest order. Budget in characters (0 = unlimited): degrade the level of ALL remaining tools one step (full→summary→index) and re-render until it fits; only once every tool is at index level and still over budget, drop whole tools from the tail — a partial description of every tool beats a full description of half of them (§7.1 rationale). Disabled/unavailable tools never render. Example shape:

    ## Tools
    ### fs 1.0.0 — Files in the workspace
    - fs.read {path, offset?, max_bytes?, encoding?}  [read-only]
        Read a file. Returns {content, size, truncated}.
        e.g.  CALL fs.read {path: "notes/todo.txt"}

Markers: [read-only] [destructive] [idempotent] [long-running] [deprecated].

### 7.2 Call lines
`CALL <tool>[@<version>].<command> {<single-line xCDN object>}` — first well-formed call line wins; strings escape newlines. Result lines: `RESULT <tool>.<command> {…}` / `ERROR <tool>.<command> {code: "…", message: "…"}`.

### 7.3 GBNF
Exported grammar = exactly the valid call lines of the enabled registry: one production per (tool,command), literal names, argument productions from param types, enums as literal alternatives, required-before-optional in manifest order (canonical order in grammar; parser accepts any order). Grammar guarantees syntax only. llama.cpp is NOT a dependency.

### 7.4 JSON Schema (MCP)
Params → `{type:"object", properties, required, additionalProperties: false}`; constraints carried (minLength/maxLength/minimum/maximum/enum/items/minItems/maxItems/additionalProperties for map); defaults included; descriptions preserved; bytes → {type: string, contentEncoding: "base64"}; datetime/duration/uuid → format date-time/duration/uuid; path → string with semantics in description. MCP tool description = tool summary + command description + rendered examples + permission note. Deterministic.

## 8. Standard suite (astools-std)
Eight ordinary packages; one busybox-style C99 binary dispatched on `--tool <id>`; each package ships its own copy. Zero special treatment.

### 8.1 fs (perms: fs [{path:${workspace}, access: read-write}])
- read {path(read,must_exist), offset?=0, max_bytes?=65536, encoding?∈{utf8,base64}=utf8} → {content,size,truncated} [read-only, idempotent]
- write {path(write), content, mode?∈{create,overwrite,append}=create, encoding?=utf8} → {bytes_written} [destructive; create fails if exists]
- list {path(read), recursive?=false, glob?, max_entries?=1000} → {entries:[{name,type,size,modified}], truncated} [read-only]
- stat {path(read,must_exist)} → {type,size,modified,readonly} [read-only, idempotent]
- mkdir {path(write), parents?=true} → {created} [idempotent]
- remove {path(write,must_exist), recursive?=false} → {removed} [destructive; dir needs recursive:true]
- move {src(write,must_exist), dst(write)} → {} [destructive]
- copy {src(read,must_exist), dst(write), recursive?=false} → {bytes_copied} Errors: fs/not-found, fs/exists, fs/not-empty, fs/is-dir, fs/io.

### 8.4 grep (A1; perms: fs read on ${workspace})
- search {pattern(string,min_len 1), path?(path,read)=".", regex?=true, case_sensitive?=true, glob?(string), recursive?=true, context?(int 0–10)=0, max_results?(int 1–1000)=200, max_file_bytes?=1048576} → {matches:[{path,line,column,text,before?, after?}], files_searched, files_skipped, truncated} [read-only, idempotent] Line-based; text capped 512 bytes with marker; binary files (NUL in first 4KiB) and oversized files skipped and counted; stops at max_results with truncated:true; result paths workspace-relative. Error: grep/bad-pattern.

### 8.5 edit (A1; perms: fs read-write on ${workspace})
- replace {path(rw,must_exist), find(min_len 1), replace_with, all?=false, regex?=false} → {replacements, size} [destructive]. all:false requires exactly one occurrence: 0 ⇒ edit/not-found, ≥2 ⇒ edit/ambiguous (count in message).
- insert {path(rw,must_exist), line(int,min 0), content} → {size} [destructive]. line 0 prepends; past-EOF appends.
- patch {patch(min_len 1), strip?(int,min 0)=1} → {files_changed, hunks_applied} [destructive]. Unified diff; paths resolved workspace-relative after strip, full §6.4 pipeline; any failing hunk aborts all (restore from in-memory copies); edit/patch-failed. All: write-to-temp + atomic replace; UTF-8 text, preserve dominant LF/CRLF; binary ⇒ edit/binary. Errors also: edit/bad-pattern.

### 8.6 git (A1; perms: fs rw ${workspace}, proc: true — inert until the operator grants proc).

Wraps system git; constructed argv, no shell; refs validated (check-ref-format-ish) and passed after literal `--`. Env: scrubbed + GIT_TERMINAL_PROMPT=0, GIT_CONFIG_NOSYSTEM=1, HOME=${scratch}. Local-only: no fetch/pull/push/clone/reset/rebase; checkout refuses dirty discard (git/dirty). Missing git ⇒ git/not-installed (tool stays visible).
- status {} → {branch, detached, ahead, behind, staged:[{path,change}], unstaged:[{path,change}], untracked:[path], clean} [read-only]
- diff {path?(read), staged?=false, context?(0–20)=3} → {diff, truncated}
- log {max_count?(1–200)=20, path?(read)} → {commits:[{hash, short_hash, author, date, subject}]} [read-only, idempotent]
- show {ref, path?(read)} → with path {content,size,truncated}; without {hash,author,date,subject,body,diff,truncated} [read-only, idempotent]
- add {paths(array<path rw>, min_items 1)} → {added}
- commit {message(1–4096), all?=false} → {hash, short_hash, files_changed} [destructive]
- checkout {ref, create?=false} → {branch, detached} [destructive] Errors: git/not-installed, git/not-a-repo, git/dirty, git/bad-ref, git/conflict, git/failed.

### 8.7 sys, env, proc, net
- sys.info {} → {os, arch, hostname, cpus, memory_total_mb, astools_version} [read-only; no permissions]
- env.get {name} / env.list {} — only vars covered by the env grant; ungranted report as absent (no oracle).
- proc.run {argv(array<string> min 1), cwd?(path,read), stdin?, env?(map<string>), timeout?(duration)} → {exit_code, stdout, stderr, stdout_truncated, stderr_truncated, duration} [destructive, long_running]. Direct exec, no shell. Requires proc grant (off by default). Spawned program inherits invocation sandbox/process group.
- net.fetch {url, method?∈{GET,HEAD}=GET, headers?(map), max_bytes?=262144} → {status, headers, body, truncated}. Optional artifact (ASTOOLS_BUILD_NET, links libcurl).

## 9. Concurrency
Supervisor thread per context owns persistent tool processes, idle/backoff lifecycles, registry poll. Registry table under one rwlock; invocations pin descriptors by refcount, hold no lock while a tool runs. Admission = counting semaphore (max_concurrent, default 4). astools_invoke blocks the caller; astools_invoke_async returns a task. ASTOOLS_NO_THREADS: sync oneshot only; persistent/async ⇒ ERR_UNSUPPORTED; polling via astools_tick. fork() with open context = UB.

## 11. MCP server (astools-mcp)
`astools-mcp --root <dir> [--config <file>] [--workspace <dir>]`; stdio JSON-RPC 2.0; MCP lifecycle (initialize, tools/list, tools/call, notifications/tools/list_changed). Tool naming `<tool>_<command>` (bijective, D7). tools/list: composed description, generated inputSchema, annotations readOnlyHint/destructiveHint/idempotentHint/openWorldHint(net). tools/call → astools_invoke; tool-level failure ⇒ result isError:true with code+message; engine failures ⇒ JSON-RPC errors with astools_err name in data. Management tools only when mcp.management: astools_registry_refresh, astools_tool_manifest, astools_sandbox_caps, astools_stats. In-house strict RFC 8259 JSON (UTF-8 only, depth-capped). Plugin behaviors (A2 §A2.3.4): 1) missing --config file ⇒ builtin defaults + info log (never create); `--config-init` writes commented default template and exits. 2) workspace resolution: --workspace ▷ ASTOOLS_WORKSPACE ▷ config workspace.root ▷ cwd. 3) installation registry root: `<exedir>/../share/astools/tools` (POSIX) / `<exedir>\tools` (Windows) prepended at trust standard when it exists. Precedence: install root ▷ ASTOOLS_PATH ▷ registry.paths.

## 12. Configuration `#astools_config` (all keys optional; defaults shown)
registry: { paths: [{path:"tools", trust:"standard"}], watch:"poll", poll_interval:r"PT5S", pinning:"warn", lock_path:"astools.lock.xcdn", pins:[] } · workspace: { root:".", scratch:null } · sandbox: { default_level:"basic", strict_fallback:"degrade", allow_library:false } · grants: { workspace_access:"read-write", tools:[] } · invocation: { timeout:r"PT30S", max_concurrent:4, max_output_bytes:1048576, stderr_max_bytes:262144, result_validation:"warn" } · catalog: { level:"summary", char_budget:8000, priority:[] } · mcp: { management:false } · logging: { path:null, level:"info", max_size_kb:5120, max_files:3, sync:false, audit:false }.

## 14. Platform notes
POSIX first. Atomic replace only rename primitive. LF written (CRLF accepted). All astools-controlled names ASCII lowercase. macOS Seatbelt via astools-jail (deprecated API, functional); RLIMIT_AS unreliable on macOS (best effort). Windows: Job Objects, restricted tokens, UTF-16 conversions — deferred implementation, honest stubs.

## 16. Errors, logging, observability
Return codes + per-context astools_last_error. Engine failure ≠ tool failure (never conflated). Library never aborts, never writes to stdout/stderr on its own. Log records: `<RFC3339> LEVEL SUBSYS  message`; subsys ∈ registry, manifest, invoke, proc, sandbox, catalog, mcp, config, policy, worker. Levels error/warn/info/debug; info logs every invocation with outcome and timing. Size-based rotation (max_size_kb, max_files); logging.sync ⇒ fsync per line. Logging failures never fail the operation. Audit (logging.audit): one `#invocation {at, tool, version, command, ok, error, duration_ms, args_sha256}` per invocation appended to audit.xcdn; args hashed, not stored; append-only, never rotated.

## 17. Testing
CTest + in-house harness (tests/astools_test.h). Unit: type checker (every kind/constraint), semver, path canonicalization (traversal + symlink attacks), policy intersection, lockfile modes, JSON codec, xCDN round-trips, regex engine (constructs + adversarial linear-time), call-line parser. Scripted fake tools (echo, sleep, crash, flood, ignore-cancel, bad-protocol) drive the invocation pipeline deterministically. Escape suite: read/write outside grants, deadline overrun, flood — assert containment exactly where the platform matrix claims it. Golden files: fixed registry ⇒ byte-identical catalogs (3 levels), GBNF, JSON Schemas. Injected clock for deadlines/backoff/idle/poll.

## A1.6 Regex engine (in-house, D13)
Documented ERE subset compiled to a backtracking-free simulation (linear time O(pattern × input)): literals, `.`, `[…]`, `[^…]` (ranges, classes), `^`, `$`, `*`, `+`, `?`, `{m,n}`, alternation `|`, grouping `()`. NO backreferences, NO lookaround. Case-insensitive flag. Platform-identical; no <regex.h>, no PCRE.

## A2.3 Agent Plugin (astools-plugin)
Layout: plugin.json + mcp.json + skills/astools/SKILL.md + LICENSE. plugin.json: {$schema: "https://agent-plugins.org/schemas/1.0.0/plugin.schema.json", name: "astools", version: <release>, description, license: "MIT", homepage, keywords}. mcp.json: {$schema: ".../1.0.0/mcp.schema.json", mcpServers: {astools: {type: "stdio", command: "astools-mcp", args: ["--config", "${PLUGIN_DATA}/config.xcdn"], cwd: "${PLUGIN_DATA}"}}} — bare command (PATH strategy, D17), no env block. astools-check --plugin-lint validates: exactly one stdio server named astools, bare command astools-mcp, cwd ${PLUGIN_DATA}, no env, name constraints, $schema ids.

## 19. Key decisions
D1 oneshot canonical · D3 xCDN-C parses all persistent/wire text · D4 1 tool = 1 package = 1 manifest · D5 basic default, strict opt-in + honest caps · D6 SHA-256 lockfile (no signatures in v1) · D7 slug charset forbids `_` ⇒ `<tool>_<command>` bijective · D8 strict validation, defaults injected · D9 no network in libastools · D10 no derived binary state · D13 in-house linear-time regex · D14 git wraps system binary, local-only, needs proc grant · D15 edit exactly-once default + atomic all-or-nothing · D16 global compatibility-free rename to astools; CI lint: zero matches of the legacy slug · D17 plugin PATH strategy · D18 ${PLUGIN_DATA} anchors all mutable state.

## Appendix B — wire protocol
Request (runtime→tool stdin):

    #tool_request {
      protocol: 1,
      invocation_id: u"…",
      tool: "fs", version: "1.0.0", command: "read",
      args: { path: "/abs/canonical", offset: 0, … },
      deadline: t"2026-08-02T10:15:32Z",
      limits: { max_output_bytes: 1048576 },
      grants: { fs: [{path: "/abs", access: "read-write"}], net: false,
                proc: false, env: [] },
      workspace: "/abs/workspace",
      scratch: "/abs/workspace/.astools/tmp/<id>",
    }

Response (tool→runtime stdout): `#tool_response { protocol: 1, invocation_id: u"…", ok: true, result: … }` or `{ …, ok: false, error: { code: "fs/not-found", message: "…", retriable: false } }`. Persistent extras: `#tool_hello { protocol: 1, tool, version, parallel }`, `#tool_cancel { invocation_id }`.
