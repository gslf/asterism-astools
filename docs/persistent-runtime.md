# Persistent tool runtime

An executable with `runtime.mode: "persistent"` uses the ordered Astools line
protocol by default. `runtime.protocol: "mcp"` selects the
[reviewed stdio client](mcp-client.md), whose discovery and result protocol is
distinct from native `#tool_hello` exchanges. `runtime.protocol: "lsp"` selects the optional
[semantic navigation adapter](lsp.md), which has its own handshake and closes
failed exchanges by terminating the server. The runtime starts an instance, validates `#tool_hello`, then exchanges
one request and matching response at a time. A manifest's `parallel` limit permits
separate instances; it does not multiplex overlapping requests on one stream.

Instances belong to one context and immutable package identity, including the
package path and the registry content hash. Replacing a manifest or a hashed entry
cannot reuse an old executable. After both global admission and per-instance
waiting, the runtime rechecks registry availability and content before dispatch.
The supervisor retires revoked or changed idle instances on its next pass. An
already executing request is not retroactively undone by a registry change.
External full-trust executables and interpreter dependencies need separate version
policies: the package hash does not identify all of their bytes.

The instance owns its sandbox, working directory, HOME and TMPDIR until it stops.
Every request reports that same scratch directory. Environment grants are captured
when the process starts. Per-request identity and deadlines come from the request,
not from the instance's initial environment. A process replacement gets a fresh
scratch directory, and termination cleans the old one after reaping the child.

Both admission queues observe cancellation and the request's remaining deadline.
Startup uses the earlier of that deadline and `startup_timeout`. Expiring the
request during handshake reports timeout; a malformed or independently expired
handshake reports a protocol failure. A request that never acquired an instance
cannot write to its input. Once a request has reached the process, cancellation
allows the protocol's bounded acknowledgement grace before killing the group.
Malformed output or a response for another request fails the exchange as a
protocol error and retires the stream. No failed or interrupted request is
automatically replayed.

A context retains at most 64 process/library nodes, including restart backoff and
loaded libraries. At capacity, a new identity returns `ASTOOLS_ERR_BUSY` without
spawning. Idle process nodes are reclaimed; libraries stay until context close.
`astools_get_stats` and the MCP stats resource expose `instance_slots` and
`instance_waiters`, separately from global invocation `active` and `queued`.
These counters are observations, not an atomic snapshot of every subsystem.
ABI 5 adds these fields to the public statistics structure.

Native libraries remain explicitly enabled, full-trust, in-process code. The
runtime validates their response invocation ID. A changed package at an already
loaded path is rejected until context reopen, because a loader may retain the
previous image. The ABI cannot interrupt a blocked native callback or contain its
crash; its deadlines remain advisory.

The POSIX shutdown path kills remaining members of the process group even when
the leader exits cooperatively. This is not containment of descendants that escape
the group: strict sandbox enforcement and platform capability reports still apply.
The new lifecycle tests run actual Linux processes in basic and available strict
sandbox modes, including package replacement, scratch reuse, queue expiry and
cancellation, revocation and a descendant that ignores SIGTERM. Separate tests load
an actual shared library. Windows/macOS have not been validated by these runs.

This runtime is a persistent **tool protocol**, not yet an interactive shell or
REPL session API. Raw-process IDs, separate output cursors, streaming input, and
restart-visible job state require a host-owned controller. Tool subprocess stdout
is protocol data; stderr is currently drained during exchanges, not retained as a
user-addressable stream.
