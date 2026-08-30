# astools — Architecture and Design

## 1. Why this project exists

Language models need tools to inspect and change the real world, but exposing a
shell or an untyped collection of callbacks creates three problems: the model
must guess syntax, the host cannot reason clearly about permissions, and large
tool descriptions consume the context before useful work begins.

astools is a local tool registry built around a stronger contract:

> A tool is a self-describing package. The same manifest defines what the model
> may ask for, what the runtime will validate and what the host may authorize.

The registry turns model intent into a small, typed action language and turns
execution back into structured evidence.

## 2. Place in Asterism

The four projects divide responsibilities as follows:

- **asmodel** runs model providers.
- **Asper** owns durable memory and bounded context.
- **astools** owns tool discovery, contracts, policy and execution.
- **asngn** decides when a tool is needed and evaluates its outcome.

astools does not decide an agent plan and does not trust a model's claim that an
action succeeded. It validates and executes a requested operation, then returns
the actual result for asngn to judge and, when useful, Asper to retain.

## 3. A tool is one package

A tool is a directory containing a `manifest.xcdn` and its implementation. The
manifest is the single source of truth for:

- tool identity and version;
- commands and conversational descriptions;
- typed parameters, defaults and constraints;
- worked examples shown to a model;
- requested filesystem, process, environment or network permissions;
- invocation mode and executable location;
- output and execution limits.

This avoids separate schemas for documentation, model prompting and runtime
validation. A manifest change updates all three views together.

Implementations may be one-shot executables, supervised persistent processes or
in-process library handlers. One-shot execution is the canonical portable path.

## 4. Registry architecture

The host opens a registry with ordered roots, workspace information, trust rules
and grants. A scan validates packages, resolves versions and builds an immutable
registry generation.

Refresh is atomic: readers observe either the previous complete generation or
the next complete generation, never a half-scanned registry. Lockfiles and pins
allow an operator to approve exact package identities where reproducibility is
required.

Invocations pin their resolved descriptor, release the registry read lock and
then run. A concurrent refresh therefore does not block a long tool call or
invalidate the command already selected.

## 5. Invocation pipeline

Every call follows the same ordered pipeline:

1. Resolve the tool and command in the current registry generation.
2. Parse the argument object.
3. Validate types, required fields, defaults and declared constraints.
4. Canonicalize every path-valued argument.
5. Intersect manifest requests with host grants and reject missing authority.
6. Dispatch through the declared execution mode.
7. Supervise cancellation, deadline, process tree and output limits.
8. Validate the returned result and attach structured diagnostics.
9. Emit audit and statistics data.

Arguments are not coerced to make a malformed call appear valid. Unknown fields,
wrong types and invalid paths fail before the implementation runs. Predictable
errors teach a small model more effectively than permissive behavior whose
meaning changes from call to call.

## 6. Conversational interface

The same registry can be projected in three catalog levels:

- **Index:** tool and command names for discovery.
- **Summary:** enough contract detail to select a command.
- **Full:** parameter schemas, constraints and examples needed to call it.

A character budget applies to the complete catalog. When space is tight,
astools reduces detail across all tools before dropping tools, preserving broad
discoverability.

The compact native action syntax is one line:

```text
CALL code.read-range {path: "src/main.c", start: 40, end: 90}
```

The response is a structured `RESULT` or `ERROR`. astools generates GBNF for the
native syntax and JSON Schema for MCP clients from the same manifests. Grammar
constraining makes syntactically impossible actions unreachable during decoding,
while runtime validation remains the authority for semantics and permissions.

## 7. Security model

The policy is deny by default. A manifest requests capabilities; the host grants
capabilities; the effective permission set is their intersection. A tool cannot
grant itself authority by editing its description.

Before spawning any executable, astools:

- canonicalizes paths and checks them against workspace grants;
- rejects traversal and out-of-scope access;
- builds a scrubbed environment and controlled `PATH`;
- uses a private scratch working directory;
- avoids shell interpretation for standard process and project operations;
- places child processes under supervision as a group;
- applies time, output and platform resource limits.

The basic sandbox enforces these rules in the runtime. Strict mode adds kernel
facilities where the operating system provides them. Capability reporting is
honest: the API reports the enforcement actually active on the platform rather
than claiming a portable sandbox is stronger than it is.

Potentially destructive operations remain visible to asngn, which can require
human confirmation before invocation.

## 8. Semantic tools for software work

The standard suite includes conventional filesystem, search, edit, git,
process, system and environment operations. Its most important architectural
choice is a higher-level coding surface:

- `code.*` reads ranges, searches symbols and applies patches;
- `project.*` builds, tests, lints, formats and collects diagnostics.

These commands express intent without giving the model an executable string or
arbitrary argument vector. The model selects a bounded operation; the tool owns
platform-specific command construction and result normalization.

This reduces both danger and reasoning burden. A model does not need to invent a
shell pipeline, escape it correctly, adapt it to three operating systems and
interpret unstructured output just to run a test suite.

## 9. Performance strategy

astools is designed to keep registry and invocation overhead below the work a
tool performs:

- manifests are parsed and validated during scan, not for every call;
- registry generations are immutable and swapped atomically;
- invocation descriptors are pinned without holding a registry lock while work
  runs;
- a semaphore bounds concurrent external processes;
- persistent implementations can amortize startup where appropriate;
- one-shot execution remains available for isolation and simple deployment;
- output caps prevent unbounded allocation and downstream prompt growth;
- catalogs and grammars are generated from already parsed definitions;
- the standard semantic tools use direct process execution rather than a shell.

The result is predictable latency under concurrent use without weakening the
validation boundary.

## 10. Token economy

Tool integration can consume more tokens describing and repeating tools than
executing the user's task. astools controls that cost structurally:

- catalog levels disclose only the detail needed at the current planning stage;
- a global character budget bounds prompt footprint;
- concise manifests provide names, types and examples without prose duplication;
- generated grammar removes the need for repeated syntax-repair conversations;
- defaults omit routine arguments from calls;
- semantic commands replace long shell instructions with short intent objects;
- structured errors identify the exact invalid field instead of returning a
  large ambiguous failure transcript;
- output caps keep a verbose subprocess from consuming the next model context.

When a valid result is still large, asngn keeps a short digest in context and
stores the exact payload as an Asper object. astools does not silently truncate
success evidence and pretend nothing was omitted; it reports limits explicitly
so the harness can preserve and reopen the source.

## 11. How this helps small language models

Small models are less reliable when they must remember tool syntax, choose among
dozens of similar commands and recover from permissive but surprising APIs.
astools externalizes those difficulties:

1. A compact catalog narrows tool selection.
2. Types and examples make valid arguments easier to construct.
3. Grammar constraining eliminates malformed call syntax during decoding.
4. Strict validation turns hallucinated fields into precise, actionable errors.
5. Semantic coding commands reduce the number of decisions per operation.
6. Structured results provide evidence that the orchestrator can test against a
   declared success condition.

The model spends its limited capacity deciding what should happen. Deterministic
code handles parsing, permissions, platform differences and process control.

## 12. Concurrency and lifecycle

The registry uses reader/writer synchronization around generation replacement.
Calls in flight own stable descriptors. External work is bounded by configured
concurrency and supervised independently, so one stalled command does not hold a
global registry lock.

Cancellation and deadlines terminate the supervised process group rather than
only the immediate child. Output collection remains capped across stdout and
stderr. Async APIs expose this lifecycle to hosts without requiring one thread
per model turn.

## 13. Fundamental invariants

The implementation must preserve these rules:

1. The manifest is the single contract for model, runtime and operator.
2. Registry refresh is atomic.
3. Input is validated before implementation code runs.
4. Manifest permission requests never exceed host grants.
5. Paths are canonicalized before authorization.
6. The standard tool path does not invoke a shell implicitly.
7. Grammar constrains syntax; runtime policy remains authoritative.
8. Output and process execution are bounded and cancellation is observable.
9. Platform sandbox capability is reported truthfully.
10. Tool results are evidence, not claims of task success.

## 14. Public surfaces

`include/astools.h` is the authoritative C99 API. It covers registry lifecycle,
catalog and grammar generation, call parsing and formatting, synchronous and
asynchronous invocation, sandbox capabilities, readiness and statistics.

`astools-mcp` exposes registered commands as MCP tools using generated JSON
Schema. `astools-check` validates packages and previews their model-facing
catalog. The standard suite and plugin are ordinary consumers of the same
package model.

Build instructions belong in the README. The exact manifest format and authoring
workflow belong in `docs/AUTHORING.md`; this document defines the system design
and the invariants every package must respect.
