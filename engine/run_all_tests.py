"""Compile and run every test_*.cpp in this folder, then build the engine program.

Run (from the engine folder):
    py run_all_tests.py                  Windows (py launcher)
    python3 run_all_tests.py             Linux / macOS

Options:
    --cxx g++          compiler to use (default: g++)
    --std c++14        language standard (default: c++14)
    --only dijkstra    only run test files whose name contains this text
    --no-cli           do not build engine_cli
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent


def run(cmd, timeout):
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          timeout=timeout, cwd=str(HERE))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cxx", default="g++")
    ap.add_argument("--std", default="c++14")
    ap.add_argument("--only", default="")
    ap.add_argument("--no-cli", action="store_true")
    args = ap.parse_args()

    try:
        version = subprocess.run([args.cxx, "--version"], stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT).stdout.decode(errors="replace").splitlines()[0]
    except (OSError, IndexError):
        print(f"Could not run '{args.cxx} --version'. Is the compiler installed and on PATH?")
        return 2
    print(f"compiler: {version}   standard: {args.std}\n")

    exe_suffix = ".exe" if os.name == "nt" else ""
    tests = sorted(p for p in HERE.glob("test_*.cpp") if args.only in p.name)
    if not tests:
        print("no test files found")
        return 2

    results = []
    with tempfile.TemporaryDirectory() as tmp:
        for src in tests:
            out = Path(tmp) / (src.stem + exe_suffix)
            t0 = time.time()
            build = run([args.cxx, f"-std={args.std}", "-O2", str(src), "-o", str(out)], timeout=300)
            if build.returncode != 0:
                results.append((src.name, "COMPILE ERROR", build.stdout.decode(errors="replace")))
                continue
            try:
                test = run([str(out)], timeout=600)
            except subprocess.TimeoutExpired:
                results.append((src.name, "TIMEOUT", ""))
                continue
            text = test.stdout.decode(errors="replace")
            m = re.search(r"passed:\s*(\d+)\s+failed:\s*(\d+)", text)
            took = time.time() - t0
            if test.returncode == 0 and m and m.group(2) == "0":
                results.append((src.name, f"ok  {m.group(1)} checks  ({took:.1f}s)", ""))
            else:
                results.append((src.name, "FAILED", text))

    width = max(len(r[0]) for r in results)
    for name, status, _ in results:
        print(f"  {name:<{width}}  {status}")
    bad = [r for r in results if not r[1].startswith("ok")]
    for name, status, detail in bad:
        print(f"\n----- {name}: {status} -----\n{detail[-3000:]}")

    if not args.no_cli:
        cli = HERE / ("engine_cli" + exe_suffix)
        build = run([args.cxx, f"-std={args.std}", "-O2", "engine_cli.cpp", "-o", str(cli)], timeout=300)
        if build.returncode == 0:
            print(f"\nbuilt {cli.name}")
        else:
            print("\nFAILED to build engine_cli:\n" + build.stdout.decode(errors="replace")[-3000:])
            bad.append(("engine_cli.cpp", "COMPILE ERROR", ""))

    print("\nALL GOOD" if not bad else f"\n{len(bad)} problem(s)")
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main())
