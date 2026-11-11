#!/usr/bin/env python3
"""Reproducible CPU vs GPU compiler timings; outputs stay below bin/."""
import argparse
import json
import os
from pathlib import Path
import statistics
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]


def source(functions, operations):
    def signed32(value):
        return (value + 2**31) % 2**32 - 2**31

    def expected(value):
        for i in range(operations):
            value = signed32(signed32(value * 33) + i + 1) ^ (value >> 3)
        return value

    parts = []
    for f in range(functions):
        body = [f"fn work_{f}(x: i32) i32 {{", "    let result = x"]
        for i in range(operations):
            body.append(f"    result = (result * 33 + {i + 1}) ^ (result >> 3)")
        body.extend(["    return result", "}"])
        parts.append("\n".join(body))
    # Every helper stays in the module and is compiled, even without DCE.
    parts.append(f"fn main() {{ assert work_0(17) == {expected(17)}\n"
                 f"assert work_{functions - 1}(-17) == {expected(-17)} }}")
    return "\n".join(parts) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repeats", type=int, default=9)
    parser.add_argument("--ns", type=Path, default=ROOT / "bin/compiler-release/ns")
    args = parser.parse_args()
    ns = args.ns.resolve()
    bench = ns.parent / "ns_compiler_bench"
    out = ROOT / "bin/bench/compiler"
    out.mkdir(parents=True, exist_ok=True)
    # An isolated tool build still needs the native runtime source/header SDK
    # beside its root. Only add benchmark SDK links below this repo's bin/.
    if ns.is_relative_to(ROOT / "bin") and ns.parent != ROOT / "bin":
        sdk = ns.parent.parent / "share/ns-runtime"
        sdk.mkdir(parents=True, exist_ok=True)
        for name in ("src", "include"):
            link = sdk / name
            if not link.exists():
                link.symlink_to(ROOT / name, target_is_directory=True)
    env = os.environ.copy()
    for key in ("NS_COMPILER_BACKEND", "NS_COMPILER_GPU_VERIFY", "NS_COMPILER_GPU_LIBRARY"):
        env.pop(key, None)
    rows = []
    for name, functions, operations in [("small", 16, 32), ("medium", 128, 64), ("large", 512, 128)]:
        path = out / f"{name}.ns"
        path.write_text(source(functions, operations))
        measured = subprocess.run([str(bench), str(path), str(args.repeats)], cwd=ROOT, env=env,
                                  capture_output=True, text=True, check=True)
        row = json.loads(measured.stdout.strip().splitlines()[-1])
        row["name"] = name
        wall = {"cpu": [], "gpu": []}
        # Full forced compile includes process start, parse, SSA, emission,
        # runtime preparation and system linking. Each process has a cold
        # compiler GPU pipeline cache; OS/driver caches may already be warm.
        for repeat in range(args.repeats):
            for mode in (("cpu", "gpu") if repeat % 2 == 0 else ("gpu", "cpu")):
                run_env = dict(env, NS_COMPILER_BACKEND=mode)
                target = out / f"{name}_{mode}"
                start = time.perf_counter()
                result = subprocess.run([str(ns), "build", "--force", "--exe", str(path), "-o", str(target)],
                                        cwd=ROOT, env=run_env, capture_output=True, text=True, check=True)
                wall[mode].append((time.perf_counter() - start) * 1000)
                if "using CPU last-use analysis" in result.stdout + result.stderr:
                    raise RuntimeError("GPU benchmark silently fell back to CPU")
        for mode in ("cpu", "gpu"):
            row[f"{mode}_build_ms"] = statistics.median(wall[mode])
            subprocess.run([str(out / f"{name}_{mode}")], cwd=ROOT, check=True)
        rows.append(row)
        print(f"{name:8} analysis CPU/GPU {row['cpu_analysis_ms']:.3f}/{row['gpu_analysis_ms']:.3f} ms; "
              f"backend {row['cpu_backend_ms']:.3f}/{row['gpu_backend_ms']:.3f} ms; "
              f"build {row['cpu_build_ms']:.1f}/{row['gpu_build_ms']:.1f} ms", flush=True)
        (out / "results.json").write_text(json.dumps({"repeats": args.repeats, "results": rows}, indent=2) + "\n")


if __name__ == "__main__":
    main()
