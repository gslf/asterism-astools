# Command discovery and execution snapshots

`astools_discover` produces one immutable command selection for a live context.
Catalog, GBNF and JSON Schema use the same sequence. Each entry includes its bare
`tool.command` name, exact `tool@version` reference, parameter schema, summary and
package content hash. Configuration pins now govern all three exports as well as
invocation; an unavailable pinned version cannot expose a different version's
schema under the same bare name.

The selector ranks names and summaries against at most 32 query terms. Matching
folds ASCII case and retains Unicode bytes; it is a bounded lexical policy, not a
semantic or multilingual quality claim. Host allowlists restrict the candidates.
The read-only option narrows command annotations without creating OS permissions.
Disabled/unavailable runtimes, unmet mandatory process/network grants and impossible required
path grants are excluded. Optional argument values and actual containment remain
subject to invocation policy. Manifest descriptions and capability claims never
grant authority.

Limits are 64 selected commands, a 4096-byte query and a 1 MiB schema budget.
Defaults select up to 16 commands under 24000 bytes including schema/summary/name
reserves. A bounded 64-entry shortlist is ranked before fitting the final budget;
entries that cannot fit are omitted as units. `astools_selection_omitted` reports
eligible candidates outside the final selection. Clients can issue a more
specific query or allowlist to expand another command. The compact catalog lists
only selected command signatures; the separate schema getter supplies details.
⁂ asngn exposes a bounded `discover` action that replaces the current selection.

Selections own their strings and retain manifest descriptors. Their context must
outlive them and any submitted tasks. A process-local registry revision changes
on content/availability updates and host enable/disable operations; identical
refreshes keep it stable. Clients can cache by context, revision and all discovery
options. No cross-process/persistent selection cache is implied.

## Checked invocation

`astools_selection_validate` checks identity, arguments and host policy without
executing. Hosts use it before confirmations or reuse of cached results. Validation
is an observation, not a persistent authorization grant.

`astools_selection_invoke` and its asynchronous counterpart accept only selected
bare command names. They resolve the selected exact version, validate arguments,
apply current host policy and use the existing sandbox/dispatch pipeline. After
waiting for a slot, availability and identity are checked again. Changed manifests
or package-relative runtime entry bytes are rejected even without a registry
refresh. A disabled command cannot execute through an older selection.

The asynchronous call copies its reference and expected hash before returning,
so the caller can free the selection immediately after submission. Queue waits
observe cancellation and deadlines, including the per-instance wait of persistent
tools. The [persistent runtime](persistent-runtime.md) retains each instance's sandbox
and refuses stale executable reuse. `astools_get_stats` and the MCP stats resource
report current active/queued counts. Validation, queue time and final identity
checks consume the invocation deadline.

The content identity covers the manifest and package-relative runtime entry
artifacts. It is not a complete toolchain/environment identity. Full-trust external
executables, interpreter dependencies and arbitrary repository files require their
own version policy. File verification and process creation are not an atomic
operation against arbitrary external writers. Pinning and OS sandbox enforcement
remain separate controls.

## Validation

Tests cover a relevant command at the end of an 82-package registry, allowlists,
insufficient budgets, impossible grants, pin mismatch, stable refreshes, revocation,
changed live bytes, actual synchronous/asynchronous invocation and release of a
selection before task completion. Queue tests hold a real slot and observe a
waiting task before cancellation, revocation or a manifest change; they do not
assume that an arbitrary sleep means the task has reached the queue.
