# deps/patches

Local fixes to the pinned `deps/xcdn-c` submodule, applied automatically at
CMake configure time (idempotent, self-detecting). The submodule gitlink is
**not** moved — it stays at the upstream commit and these patches carry the
changes.

- `0001-xcdn-c-string-escapes-and-oom.patch` — four fixes required by
  astools' threat model: string-escape decode symmetry, `grow_ptr_array` OOM
  safety, a parser nesting-depth cap against stack-overflow DoS, and freeing
  the lookahead token on the parse-error path.

Full rationale, verification, and the upstreaming path:
[`../../docs/XCDN-MODS.md`](../../docs/XCDN-MODS.md).
