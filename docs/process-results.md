# Process results and deadlines

`proc.run` executes argv directly and reports the program's exit status separately
from tool protocol success. A tool result with `ok: true` means the invocation
produced a result; inspect **both** `exit_code` and `timed_out` before considering
the program successful. A leader can exit with zero while a descendant keeps its
pipes open until the capture deadline, yielding `exit_code: 0, timed_out: true`.

On POSIX, the local deadline covers spawn acknowledgement, pipe capture and
waiting for the direct child, including children that close all three standard
streams before exiting. Capture errors and incomplete exec-status reports fail
explicitly. The child stays in the enclosing runtime's invocation process group;
the runtime owns cleanup of remaining descendants. This one-shot runner is not
an interactive process controller, and escaping process groups requires separate
OS enforcement. Other platform lifecycle checks remain in the platform matrix.

Each captured stream has a string, an encoding (`utf8` or `base64`), a captured
byte count **before encoding**, and a truncation flag. Embedded NUL bytes or
invalid UTF-8 cause base64 encoding of the entire captured stream. A UTF-8
sequence split by a byte cap is preserved the same way. A cap or forced capture
termination marks the stream possibly incomplete; no tail is silently hidden
behind a NUL. `proc.run` and each `project.*` workflow step use this same encoder.
The default capture cap remains 262144 bytes per stream. The outer invocation
also retains its output and time quotas.

Fixed durations use one shared integer parser across type validation, config,
manifests and `proc.run`: `PnW` or `PnDTnHnMnS`, with up to three decimal digits
on seconds. It rejects calendar months/years, repeated/out-of-order units,
exponents, fractional other units and sub-millisecond precision. No decimal is
silently rounded to an unlimited timeout. Manifest rendering preserves the exact
millisecond value. Positive configuration/manifest execution periods and local
timeouts are limited to 4294967294 ms, avoiding the Windows `INFINITE` sentinel
and keeping them within the public deadline range. A zero `proc.run` local
timeout delegates to the enclosing invocation deadline.

Tests use real packaged tools for early pipe closure, inherited pipe holders,
fractional timeouts, nonzero exits, stdin, missing executables, NUL/invalid UTF-8
output and NUL bytes after 4 KiB. Contract checks also cover exact decimal
conversion, overflow, period bounds and manifest round trips. These tests do not
establish interactive process persistence or complete enforcement on every OS.
