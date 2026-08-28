---
name: astools
description: Conventions for using the astools system tools — safe defaults, how to read errors, and what is off by default.
---

# Using astools

astools exposes local system tools (code, project, fs, grep, edit, sys, env,
proc, git) through one MCP server. The tool manifests are the contract: argument
types are strict (no coercion — `"3"` is not an integer), defaults are
injected for you, and every path argument is canonicalized and checked
against operator grants before anything runs.

## Safe defaults — do not fight them

- `fs.write` defaults to `mode: "create"` and fails with `fs/exists` when
  the file already exists. That guard is intentional protection against
  silent clobbering: pass `mode: "overwrite"` or `"append"` only when
  replacing the file is the actual intent.
- `edit.replace` requires exactly one occurrence of `find` by default.
  When it fails with `edit/ambiguous`, add surrounding context to `find`
  until it is unique — do not switch to `all: true` unless you really
  mean every occurrence.
- Prefer `grep.search` over reading files wholesale with `fs.read`, and
  prefer `edit.replace` / `edit.insert` / `edit.patch` over rewriting
  whole files with `fs.write`. The narrow operation fails loudly; the
  broad one destroys quietly.
- For coding work, prefer `code.read-range`, `code.search-symbol` and
  `code.apply-patch`; they expose line-, identifier- and patch-level intent
  without processes. Prefer `project.build`, `project.test`, `project.lint`,
  `project.format` and `project.diagnostics` over `proc.run`; their adapter
  enum maps to fixed argv that the model cannot alter.

## Reading errors

- `astools/denied` means an operator grant is missing. The message names
  the grant. Do not retry — the answer will not change; tell the user
  what to grant instead.
- `astools/invalid-args` names the offending parameter. Fix that
  parameter and call again; do not resend the identical call.
- `git/not-installed`, or the astools server not starting at all, means
  the host installation is incomplete. That is user action — nothing a
  different call can fix.

## Off by default

`project`, `proc.run` and the whole `git` tool need an explicit per-tool
operator `proc` grant. Their absence is policy, not a bug: a denial here is
the operator's decision, so do not retry it. Grant `project` for semantic
build workflows; that does not grant arbitrary `proc.run`. Ask for the
broader `proc` tool only when no semantic action can express the task.
