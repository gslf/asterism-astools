# Modifications to xCDN-C

astools parses and serializes all of its persistent and wire text with
[xCDN-C](https://github.com/gslf/xCDN-C) (spec decision D3), pinned as the
git submodule `deps/xcdn-c` at commit
`a292ee99c7d9d3864f204155b16720620346bef1` (`XCDN_VERSION` 0.1.0).

Bringing the library up against astools' threat model — every tool's stdout
is attacker-controlled (§6.1) and is fed to the parser on every invocation —
surfaced four defects in that pinned revision. They are fixed **locally**,
without moving the submodule pointer, so the recorded gitlink stays at the
upstream commit and the fixes travel as a reviewable patch.

## How the fixes are applied

The whole change set lives in one file:

```
deps/patches/0001-xcdn-c-string-escapes-and-oom.patch
```

CMake applies it at **configure** time (top-level `CMakeLists.txt`). The step
is idempotent and self-detecting: it greps `deps/xcdn-c/src/lexer.c` for the
buggy escape-handling pattern and only runs `git apply` when that pattern is
still present. A fresh `git submodule update --init` therefore gets patched
automatically on the first `cmake -B build`; an already-patched tree (or a
future upstream that shipped the fixes) is left untouched.

Nothing else in the build depends on manual patching — `git clone
--recurse-submodules` followed by the normal CMake flow is sufficient.

To regenerate the patch after editing the submodule working tree:

```bash
git -C deps/xcdn-c diff > deps/patches/0001-xcdn-c-string-escapes-and-oom.patch
```

## The four modifications

### 1. String escapes: parse must invert serialize (`src/lexer.c`)

**Defect.** `read_string()` decoded only `\"` and `\\`. Every other escape —
`\n`, `\r`, `\t`, `\b`, `\f`, `\/`, and `\uXXXX` — was passed through with the
backslash **kept literally** in the decoded string. The serializer
(`ser.c: write_escaped_string`) does the opposite: it *encodes* a raw newline
as `\n`, a tab as `\t`, and any control byte as `\u00XX`. Serialize→parse was
therefore not the identity: a string containing a newline came back as the
two characters `\` `n`.

**Why it mattered for astools.** The whole runtime round-trips text through
xCDN — arguments in `#tool_request`, results in `#tool_response`, catalog
examples, audit records. A model asking to write `"line1\nline2"` would have
reached the tool as the literal 12 characters `line1\nline2`, silently
corrupting data across the entire system, not just in one tool.

**Fix.** `read_string()` now decodes the full set: `\b \f \n \r \t` to their
control bytes, `\/` to `/`, and `\uXXXX` to UTF-8 — including surrogate-pair
decoding (`😀` → `😀`) and rejection of lone/invalid surrogates.
Parse now inverts serialize exactly.

Verified: a round-trip of `"a\nb\tc\"d\\eé😀"` (control chars, escaped quote,
escaped backslash, 2-byte and 4-byte UTF-8) returns byte-identical. The
library's own `tests/test_lexer.c` expectations, which asserted the old
"escape preserved" behavior, were updated to assert correct decoding (part of
the same patch).

### 2. `grow_ptr_array` must report allocation failure (`src/ast.c`)

**Defect.** The growth helper behind every AST mutator
(`xcdn_object_set`, `xcdn_array_push`, `xcdn_document_push_value`,
`xcdn_node_add_tag`, `xcdn_annotation_push_arg`) was `void` and *ignored*
`realloc` failure:

```c
static void grow_ptr_array(void **ptr, size_t *cap, size_t elem_size) {
    void *new_ptr = realloc(*ptr, new_cap * elem_size);
    if (new_ptr) { *ptr = new_ptr; *cap = new_cap; }   /* silent on failure */
}
```

Each caller then wrote one element past the **unchanged** (or `NULL`)
buffer — a heap buffer overflow under memory pressure, reachable while
building an AST from hostile input.

**Fix.** `grow_ptr_array` now returns `int` (1 success / 0 failure), and every
one of its seven call sites bails out without writing when it returns 0:

```c
static int grow_ptr_array(void **ptr, size_t *cap, size_t elem_size) {
    void *new_ptr = realloc(*ptr, new_cap * elem_size);
    if (!new_ptr) return 0;
    *ptr = new_ptr; *cap = new_cap; return 1;
}
...
if (!grow_ptr_array((void **)&doc->values, &doc->values_cap,
                    sizeof(xcdn_node_t *))) return;
```

The astools modules that build ASTs programmatically (manifest render, proto
request build, lockfile, jscm) had each carried their own checked-growth
workarounds to avoid these mutators; the fix removes the hazard at the source.

### 3. Parser depth cap against stack-overflow DoS (`src/parser.c`)

**Defect.** The recursive-descent parser had **no nesting limit**:
`parse_value → parse_object/parse_array → parse_node → parse_value`. A
compromised tool emitting a few hundred KB of `[` — well under the 1 MiB
`invocation.max_output_bytes` cap — recurses one stack frame per level and
overflows the thread stack, crashing the whole host process (verified:
SIGSEGV, exit 139, from ~200 000 levels).

This is a hostile-input DoS that violates safety goal G7 ("a misbehaving
tool can never crash the host").

**Fix.** `parser_t` gains a `depth` counter; entering an object or array
checks it against `XCDN_MAX_DEPTH` (512, far beyond any legitimate document)
and returns a normal parse error instead of recursing:

```c
#define XCDN_MAX_DEPTH 512
...
case XCDN_TOK_LBRACE:
case XCDN_TOK_LBRACKET:
    if (p->depth >= XCDN_MAX_DEPTH) { /* error "nesting too deep" */ }
    p->depth++;
    val = (t.type == XCDN_TOK_LBRACE) ? parse_object(p) : parse_array(p);
    p->depth--;
    break;
```

Verified: 100 000-deep input now returns `NULL` (rejected) instead of
crashing; a 64-deep document still parses. Regression locked in by
`tests/test_types.c: parser_depth_cap_rejects_deep_nesting` /
`parser_accepts_reasonable_nesting`.

### 4. Free the lookahead token on the parse-error path (`src/parser.c`)

**Defect (defensive).** `xcdn_parse_str` frees the partial document on error
but never frees the parser's one-token lookahead buffer (`p.look`). When
parsing errors out while a heap-owning token (string, ident, decimal, …) sits
in that buffer, the token leaks. astools feeds the parser untrusted tool
output continuously, so a leak-per-malformed-response is a slow resource drain
under a hostile or buggy tool.

**Status.** The structural gap is plain in the code (the error path returns
without touching `p.look`). It was flagged by the security review; on the
patched tree it was **not reproduced** as an active leak across ~13 crafted
malformed inputs under macOS `leaks` (the lookahead happened to be empty or
non-heap at each error point). It is fixed anyway, as cheap defensive
hardening consistent with the "tool output is hostile" threat model:

```c
xcdn_document_t *doc = parse_document(&p);
if (p.has_look) { xcdn_token_free(&p.look); p.has_look = 0; }
if (xcdn_error_is_set(&p.err)) { ... }
```

## Known upstream behavior left unchanged

- **Duplicate object keys collapse last-wins.** The parser stores object
  entries via `xcdn_object_set`, which overwrites an existing key, so
  duplicate keys in wire text are silently merged before astools can reject
  them. astools' own strict validation (`types.c`) still rejects duplicates in
  programmatically built objects, and duplicate keys in *tool arguments* are a
  malformed-input curiosity rather than a security boundary, so this is
  documented rather than patched. Rejecting duplicates in parsed text would
  require a parser change (a pre-scan or a "key already present" error in
  `xcdn_object_set`).

## Upstreaming

All four fixes are self-contained and independent of astools; they belong
upstream in `gslf/xCDN-C`. The patch file is the ready-made change set. Once a
released xCDN-C contains them, bump the submodule to that tag and delete both
`deps/patches/0001-*.patch` and the auto-apply block in `CMakeLists.txt` — the
self-detecting guard already makes a fixed upstream a no-op, so the removal is
purely cleanup.
