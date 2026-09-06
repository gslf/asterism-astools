# Parser fuzzing

Optional Clang/libFuzzer targets instrument the actual Astools and xCDN parsers
with ASan/UBSan. Normal builds gain no dependency or runtime work.

- `astools-fuzz-json`: strict JSON parsing, cloning, semantic round trips, stable
  serialization and raw UTF-8 validation, including NULs and integer boundaries.
- `astools-fuzz-manifest`: xCDN and manifest parsing, canonical render/reparse,
  duration overflow and exact millisecond round trips. No registry, file access,
  process or tool is created from the input.
- `astools-fuzz-schema`: supported MCP schema profiles, symmetric identity and
  unchanged input admission after cloning a schema/value pair. This tests the
  documented restricted profile, not full JSON Schema conformance.

Inputs are capped at 16 KiB inside every target as well as by libFuzzer. The
parser's own depth limits still apply. Release builds retain the explicit abort
checks. Allocation failures may skip a round trip; allocation fault injection,
storage, process lifecycle and concurrency need separate targets.

```sh
cmake -B build-fuzz -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DASTOOLS_BUILD_TESTS=OFF -DASTOOLS_BUILD_FUZZERS=ON
cmake --build build-fuzz --target astools-fuzz-json astools-fuzz-manifest astools-fuzz-schema -j4
mkdir -p fuzz-run/artifacts
for target in json manifest schema; do
  mkdir -p fuzz-run/$target
  cp fuzz/corpus/$target/* fuzz-run/$target/
  build-fuzz/astools-fuzz-$target -seed=1 -max_len=16384 -timeout=5 \
    -max_total_time=30 -rss_limit_mb=512 -print_final_stats=1 \
    -artifact_prefix=fuzz-run/artifacts/ fuzz-run/$target
done
```

Use disposable corpus copies: libFuzzer mutates its corpus directory. A finding
must become a deterministic regression before accepting the fix. Replay a
specific artifact by passing its path, or replay a whole corpus with `-runs=0`.
CI runs the three targets and retains failing inputs; that job has not yet run
remotely.

The [local run record](fuzzing-local.json) identifies source changes, reviewed
seeds, binaries and logs. With Clang 22.1.8, seed 1 and 30 seconds requested per
target, the final run completed 2,494,558 JSON, 90,851 manifest and 1,443,811 schema
executions without a finding. Leak detection was disabled because the local outer
sandbox blocks its inspection. These are bounded parser checks, not a security
audit, an OS enforcement result or a provider/model quality measurement.
