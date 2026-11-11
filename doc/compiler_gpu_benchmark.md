# Compiler acceleration benchmark — 2026-11-11

Apple M4 Max, 14 CPU cores / 32 GPU cores, 36 GB unified memory. Compiler built with `NS_DEBUG=0` (`-Os`); Metal service built with `-O2`. Nine repetitions, alternating CPU/GPU order, medians below. These are generated arithmetic compilation workloads, not application execution timings.

## Complete forced builds

Each measurement starts a new compiler process and includes frontend, native backend, runtime preparation and system linking. GPU compiler-service initialization is cold in each process; driver/filesystem caches may be warm. CPU is the production default.

| Helpers × assignments | CPU build (ms) | GPU-assisted build (ms) | GPU / CPU |
| --- | ---: | ---: | ---: |
| 16 × 32 | 39.22 | 79.89 | 2.04× |
| 128 × 64 | 54.72 | 96.32 | 1.76× |
| 512 × 128 | 200.59 | 246.58 | 1.23× |

## Warm native backend and analysis

These measurements reuse identical SSA and initialized Metal pipelines. Analysis totals include host packing, buffers, synchronization, readback and freeing results. Device time is GPU command duration only and is already included in the GPU total.

| Workload | CPU analysis (ms) | GPU analysis total (ms) | GPU device only (ms) | CPU native backend (ms) | GPU native backend (ms) |
| --- | ---: | ---: | ---: | ---: | ---: |
| small | 0.015 | 0.334 | 0.012 | 0.349 | 0.614 |
| medium | 0.230 | 1.421 | 0.038 | 7.401 | 8.374 |
| large | 2.137 | 6.630 | 0.403 | 95.727 | 102.990 |

The device reduction is fast, but host work and dispatch dominate. The first GPU analysis, including service initialization, took approximately 41–47 ms; initialization added about 40 ms over the warm path. This implementation does **not** accelerate complete builds. It validates the compiler-service architecture and analysis semantics, while leaving CPU selected by default.

## Lexing and parsing

The standalone lexer is a separate measurement. Parsing includes its own token scans, so lexer time and parser time are not additive. The bounded token cache is enabled by default; `NS_PARSE_TOKEN_CACHE=0` selects the original scanning path. These are parser-stage improvements, not whole-build speedups.

| Workload | Lexer (ms) | Parse without cache (ms) | Parse with cache (ms) | Parse time reduction |
| --- | ---: | ---: | ---: | ---: |
| small | 0.076 | 0.312 | 0.286 | 8.2% |
| medium | 1.124 | 5.378 | 4.970 | 7.6% |
| large | 8.278 | 36.317 | 34.413 | 5.2% |

Splitting functions into source files alone provides no parallelism in the current source-merging pipeline. File-level parsing and function-level semantic checking require independent arenas/scopes and a read-only declaration index. They are not implemented by the token cache or GPU analysis service.

## Validation

- GPU-required test passed: randomized SSA last-use parity, AArch64/AMD64 emitted-byte parity, missing-service CPU fallback.
- Parser cache tests passed: exact AST/location parity, context reuse, and invalid-input diagnostics/exit statuses.
- Full interpret/native/ns_cpu whitelist passed with `NS_COMPILER_BACKEND=gpu NS_COMPILER_GPU_VERIFY=1`; no mismatch or fallback diagnostic occurred.
- Generated native benchmark programs passed arithmetic assertions against independently computed signed-32-bit expected results.
- `make test` ran; Lens reported 36 passed and 2 failed (`ui_ffi_test.ns`, `vcs_test.ns`). Both failures were already observed before the parser-cache change. UI passed when rerun alone; Git still failed with the CPU compiler path. The full suite is not green.

See [compiler_gpu.md](compiler_gpu.md) for architecture, controls and reproduction commands. Raw run-specific data is written to `bin/bench/compiler/results.json`.
