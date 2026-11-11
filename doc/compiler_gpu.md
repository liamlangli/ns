# GPU-assisted compilation

This is an experimental **compiler acceleration** path, not a new execution
target for Nano Script programs. AArch64 and AMD64 native compilation can run
their module-wide SSA last-use analysis on Metal. Parsing, semantic analysis,
SSA lowering, register assignment, instruction emission and system linking
still execute on the CPU. Wasm and ns_cpu emission are unchanged.

## Use

On macOS, `make` builds `bin/ns_compile_gpu.dylib`; `make install` installs it
beside `ns`. The language-only host loads the compiler service on demand and
does not link Metal or the application GPU runtime.

```sh
NS_COMPILER_BACKEND=gpu bin/ns build --force --exe example.ns
NS_COMPILER_BACKEND=cpu bin/ns build --force --exe example.ns
```

CPU is the default. An unavailable library/device, allocation failure or GPU
execution failure prints a diagnostic and uses CPU analysis. For diagnosis,
`NS_COMPILER_GPU_VERIFY=1` compares every last-use result with the CPU before
emitting machine code, falling back on a mismatch. Verification is deliberately
excluded from performance measurements. `NS_COMPILER_GPU_LIBRARY` overrides the
service path. A normal incremental cache hit needs no analysis; use `--force`
to compare actual compilation rather than cache lookup.

`--profile` records the complete accelerated analysis as
`compiler::gpu_last_uses`, including loading, packing and readback.

## Analysis and correctness

The host packs all functions into definition/use events with concrete value,
block and instruction indices. Four ordered compute passes initialize the
values, count definitions, reduce last uses with integer atomics, and publish
results. A single module submission amortizes dispatch overhead across functions.
The immutable Metal pipelines are initialized once, and concurrent callers own
separate command buffers and storage. These are compiler-owned, synchronous
buffers, not per-frame application resources.

The analysis preserves the CPU rules (`ns_ssa_last_uses`): undefined or multiply
defined values are live out; cross-block and use-before-definition reads are
live out; phi reads occur on predecessor edges, including additional phi edges
and invalid-predecessor handling; ARG reads remain live through the next CALL;
ARRAY_STORE includes its index operand. Emission receives the same last-use
array as on the CPU and consumes it through the existing register cache.

Tests compare randomized SSA and edge cases against the CPU, compare native
machine-code bytes for both architectures, and exercise missing-service fallback.
Set `NS_REQUIRE_COMPILER_GPU=1` to fail the test rather than skip on hosts without
an accelerator:

```sh
make bin/ns_compile_gpu_test bin/ns_ast_cache_test bin/ns_compile_gpu.dylib
NS_REQUIRE_COMPILER_GPU=1 bin/ns_compile_gpu_test
bin/ns_ast_cache_test
sh test/ns_ast_cache_test.sh "$PWD/bin/ns"
make test
```

## Front-end work

The parser now uses a bounded, per-call token cache to avoid rescanning tokens
when recursive-descent parsing backtracks. Entries include source offset and
line state, preserving the existing parser's location behavior. There is no
shared mutable cache or retained source across parses. Disable it with
`NS_PARSE_TOKEN_CACHE=0`; it is enabled by default. Tests compare AST bytes,
source positions, context reuse, invalid-input diagnostics and exit statuses.

Merely moving functions into more files does not parallelize the current
front end: `ns_project_link` merges project source before a single `ns_ast_parse`.
A future parallel front end should retain file/function units:

1. Tokenize separate files independently. Splitting arbitrary byte ranges needs
   correct string/comment/newline boundary states; raw brace counting is unsafe.
2. Discover declarations/signatures, then parse function bodies in independent
   AST arenas. Preserve deterministic source locations and top-level order, and
   relocate node IDs when combining arenas.
3. Publish a read-only global symbol/type index; check function bodies with
   private local scopes, symbol stacks and closure-capture state. The existing
   semantic pass separates signatures from bodies but still mutates shared VM
   state, so running it concurrently without that refactor is unsafe.
4. Cache body analysis/SSA and invalidate it on relevant declaration, literal,
   imported module or compiler changes. Splitting a file should not itself alter
   the program's global initialization semantics.

CPU worker threads are a useful baseline for these irregular tasks. A GPU
lexer must additionally pay boundary discovery, compaction, dispatch and
readback costs; parallel throughput alone does not establish a faster compiler.

## Reproduce measurements

Build an isolated optimized host so debug allocator counters and assertions do
not distort the comparison. The benchmark runner creates runtime SDK links
below `bin/` for this isolated executable:

```sh
mkdir -p bin/compiler-release/src bin/compiler-release/lib
make -j8 NS_DEBUG=0 NS_BINDIR=bin/compiler-release \
  NS_LIB=bin/compiler-release/libns.a \
  bin/compiler-release/ns bin/compiler-release/ns_compile_gpu.dylib \
  bin/compiler-release/ns_compiler_bench
python3 bench/compiler/run.py --repeats 9
```

The generated small/medium/large sources have 16/128/512 helpers with
32/64/128 arithmetic assignments each. Generated sources, executables and
`results.json` stay in `bin/bench/compiler/`. Measurements alternate CPU/GPU
order and report medians. The benchmark checks last-use results and executes
both resulting programs. Front-end measurements separately report a standalone
lexer pass and parsing with/without memoization; parsing includes its own token
scanning, so those times must not be added together.

Analysis timings include preparation, allocation, synchronization and readback;
device command time is also reported separately. Native-backend timing starts
from the same prebuilt SSA and uses warm Metal pipelines. Full forced-build
timing includes process startup, parsing, SSA, native emission, runtime
preparation and system linking. Each CLI invocation has a cold compiler-service
pipeline cache, though driver and filesystem caches may already be warm.

On the initial Apple M4 Max (14 CPU / 32 GPU cores, 36 GB) measurements, GPU
last-use reduction itself is fast, but packing and submission cost more than
the CPU analysis. Cold service setup adds roughly 40–50 ms per process. The
GPU path therefore remains opt-in; these measurements do not justify enabling
it automatically. See [the benchmark report](compiler_gpu_benchmark.md) for
measured totals.
