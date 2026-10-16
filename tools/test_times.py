#!/usr/bin/env python3
"""Per-test time report from ctest.

ctest writes each test's time to a JUnit file (--output-junit). Run one test
at a time (-j1) so a time is the test's own, not the share of a busy machine.

    python3 tools/test_times.py run --preset dev [--tests-regex R] --out times.xml
    python3 tools/test_times.py show times.xml [--top N] [--by suite|test]
    python3 tools/test_times.py compare before.xml after.xml

A suite is the part of the test's name before the first "." (the GoogleTest
suite); a parameterized suite keeps its prefix whole
("Engines/ProjectImportTest"). Standard library only.
"""

from __future__ import annotations

import argparse
import statistics
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


def suite_of(name: str) -> str:
    """GoogleTest suite of a ctest name: text before the first '.'."""
    return name.split(".", 1)[0]


def read_times(path: Path) -> dict[str, float]:
    """Test name -> seconds, from a ctest JUnit file."""
    root = ET.parse(path).getroot()
    times: dict[str, float] = {}
    for case in root.iter("testcase"):
        name = case.get("name")
        if not name:
            continue
        try:
            times[name] = float(case.get("time", "0") or 0)
        except ValueError:
            times[name] = 0.0
    return times


def by_suite(times: dict[str, float]) -> dict[str, list[float]]:
    groups: dict[str, list[float]] = {}
    for name, t in times.items():
        groups.setdefault(suite_of(name), []).append(t)
    return groups


def cmd_run(args: argparse.Namespace) -> int:
    out = Path(args.out).resolve()
    cmd = ["ctest", "--preset", args.preset, "-j1", "--output-junit", str(out)]
    if args.tests_regex:
        cmd += ["--tests-regex", args.tests_regex]
    print(" ".join(cmd), file=sys.stderr)
    # ctest resolves the preset from the source tree, so run from the
    # repository root whatever the caller's directory.
    repo = Path(__file__).resolve().parents[1]
    return subprocess.call(cmd, cwd=repo)


def cmd_show(args: argparse.Namespace) -> int:
    times = read_times(Path(args.file))
    if not times:
        print(f"{args.file}: no testcase", file=sys.stderr)
        return 1
    total = sum(times.values())
    if args.by == "suite":
        rows = sorted(by_suite(times).items(), key=lambda kv: sum(kv[1]), reverse=True)
        width = max(len(s) for s, _ in rows[: args.top] or rows)
        print(f"{'suite':<{width}}  {'count':>5}  {'sum s':>9}  {'median':>8}  {'min':>8}  {'max':>8}")
        for suite, ts in rows[: args.top]:
            print(
                f"{suite:<{width}}  {len(ts):>5}  {sum(ts):>9.2f}  "
                f"{statistics.median(ts):>8.3f}  {min(ts):>8.3f}  {max(ts):>8.3f}"
            )
        print()
    top = sorted(times.items(), key=lambda kv: kv[1], reverse=True)[: args.top]
    width = max(len(n) for n, _ in top)
    print(f"{'test':<{width}}  {'s':>9}")
    for name, t in top:
        print(f"{name:<{width}}  {t:>9.3f}")
    print()
    print(f"{len(times)} tests, {len(by_suite(times))} suites, {total:.1f} s summed")
    return 0


def cmd_compare(args: argparse.Namespace) -> int:
    before = read_times(Path(args.before))
    after = read_times(Path(args.after))
    b, a = by_suite(before), by_suite(after)
    suites = sorted(set(b) | set(a), key=lambda s: sum(a.get(s, [])) - sum(b.get(s, [])))
    width = max([len(s) for s in suites] + [5])
    print(f"{'suite':<{width}}  {'before s':>9}  {'after s':>9}  {'diff s':>9}  {'n before':>8}  {'n after':>7}")
    for s in suites:
        sb, sa = sum(b.get(s, [])), sum(a.get(s, []))
        print(
            f"{s:<{width}}  {sb:>9.2f}  {sa:>9.2f}  {sa - sb:>+9.2f}  "
            f"{len(b.get(s, [])):>8}  {len(a.get(s, [])):>7}"
        )
    tb, ta = sum(before.values()), sum(after.values())
    print(f"{'total':<{width}}  {tb:>9.2f}  {ta:>9.2f}  {ta - tb:>+9.2f}  {len(before):>8}  {len(after):>7}")
    only_before = sorted(set(before) - set(after))
    only_after = sorted(set(after) - set(before))
    if only_before:
        print("\nonly in before:")
        for n in only_before:
            print(f"  {n}  {before[n]:.3f}")
    if only_after:
        print("\nonly in after:")
        for n in only_after:
            print(f"  {n}  {after[n]:.3f}")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    run = sub.add_parser("run", help="run ctest one test at a time and write a JUnit file")
    run.add_argument("--preset", default="dev", help="ctest test preset (default: dev)")
    run.add_argument("--tests-regex", help="passed to ctest --tests-regex")
    run.add_argument("--out", required=True, help="JUnit file to write")
    run.set_defaults(func=cmd_run)

    show = sub.add_parser("show", help="per-suite and per-test times from a JUnit file")
    show.add_argument("file")
    show.add_argument("--top", type=int, default=15, help="rows to print (default: 15)")
    show.add_argument("--by", choices=("suite", "test"), default="suite",
                      help="suite: suite table and top tests; test: top tests only")
    show.set_defaults(func=cmd_show)

    compare = sub.add_parser("compare", help="per-suite sums of two JUnit files")
    compare.add_argument("before")
    compare.add_argument("after")
    compare.set_defaults(func=cmd_compare)

    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
