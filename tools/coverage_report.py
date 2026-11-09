#!/usr/bin/env python3
"""Line and branch coverage of libs/ from a clang coverage build.

The `coverage` configure preset builds with -fprofile-instr-generate
-fcoverage-mapping; the `coverage` test preset makes every test process write
a raw profile under <build>/profiles. This script merges them and summarizes
what the test binaries reached under libs/.

    python3 tools/coverage_report.py clean --build-dir build/coverage
    ctest --preset coverage
    python3 tools/coverage_report.py report --build-dir build/coverage --out after.json
    python3 tools/coverage_report.py compare before.json after.json

`report` runs llvm-profdata merge -sparse over profiles/*.profraw, then
llvm-cov export -summary-only with every executable under <build>/tests and
<build>/apps as an object, restricted to <source>/libs. It writes per-file and
total line and branch counts as JSON and prints the totals. On macOS the tools
are found with xcrun (they must match Apple clang); elsewhere on PATH, an
unversioned name first, then the newest llvm-cov-NN. Exit 2 when one is
missing or there is no profile.

`compare` prints both totals and every file whose covered lines or branches
went down (a file missing from the second summary counts as a drop); exit 1
when any did. Standard library only.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

SOURCE_ROOT = Path(__file__).resolve().parents[1]
KINDS = ("lines", "branches")
SKIP_DIRS = {"CMakeFiles", "_deps", "Testing", "profiles"}

MACHO_MAGIC_64 = 0xFEEDFACF
MACHO_MAGIC_32 = 0xFEEDFACE
MH_EXECUTE = 2


def find_tool(name: str) -> str | None:
    """Path of an LLVM tool: xcrun on macOS, PATH elsewhere."""
    if platform.system() == "Darwin":
        try:
            out = subprocess.run(["xcrun", "--find", name], capture_output=True, text=True, check=False)
        except FileNotFoundError:
            out = None
        if out is not None and out.returncode == 0 and out.stdout.strip():
            return out.stdout.strip()
    found = shutil.which(name)
    if found:
        return found
    # Ubuntu's llvm packages install llvm-cov-18 and the like only.
    best: tuple[int, str] | None = None
    pattern = re.compile(rf"^{re.escape(name)}-(\d+)$")
    for d in os.environ.get("PATH", "").split(os.pathsep):
        try:
            entries = os.listdir(d)
        except OSError:
            continue
        for e in entries:
            m = pattern.match(e)
            if m and (best is None or int(m.group(1)) > best[0]):
                best = (int(m.group(1)), os.path.join(d, e))
    return best[1] if best else None


def is_executable_binary(path: Path) -> bool:
    """A Mach-O executable, or an ELF file that is not a shared library."""
    if not path.is_file() or not os.access(path, os.X_OK):
        return False
    try:
        with path.open("rb") as f:
            head = f.read(16)
    except OSError:
        return False
    if head[:4] == b"\x7fELF":
        return ".so" not in path.name
    if len(head) >= 16:
        magic, _cpu, _sub, filetype = struct.unpack("<IiiI", head)
        if magic in (MACHO_MAGIC_64, MACHO_MAGIC_32):
            return filetype == MH_EXECUTE
    return False


def find_objects(build_dir: Path) -> list[Path]:
    """Every executable under <build>/tests and <build>/apps, sorted."""
    found: list[Path] = []
    for top in ("tests", "apps"):
        root = build_dir / top
        if not root.is_dir():
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = sorted(d for d in dirnames if d not in SKIP_DIRS and not d.endswith(".app"))
            for name in filenames:
                p = Path(dirpath) / name
                if is_executable_binary(p):
                    found.append(p)
    return sorted(found)


def _stats(count: int, covered: int) -> dict:
    return {"count": count, "covered": covered,
            "percent": round(100.0 * covered / count, 2) if count else 0.0}


def summarize(export: dict, source_root: Path) -> dict:
    """Keep the files under <source_root>/libs, keyed by their relative path; totals over those."""
    libs = (source_root / "libs").as_posix() + "/"
    files: dict[str, dict] = {}
    totals = {k: [0, 0] for k in KINDS}
    for data in export.get("data", []):
        for entry in data.get("files", []):
            name = Path(entry["filename"]).as_posix()
            if not name.startswith(libs):
                continue
            rel = name[len(source_root.as_posix()) + 1:]
            summary = entry.get("summary", {})
            row = {}
            for k in KINDS:
                s = summary.get(k, {})
                count, covered = int(s.get("count", 0)), int(s.get("covered", 0))
                row[k] = _stats(count, covered)
                totals[k][0] += count
                totals[k][1] += covered
            files[rel] = row
    return {"totals": {k: _stats(*totals[k]) for k in KINDS},
            "files": dict(sorted(files.items()))}


def format_totals(summary: dict) -> str:
    parts = []
    for k in KINDS:
        s = summary["totals"][k]
        parts.append(f"{k} {s['percent']:.2f}% ({s['covered']}/{s['count']})")
    return ", ".join(parts)


def cmd_clean(args: argparse.Namespace) -> int:
    prof = Path(args.build_dir) / "profiles"
    removed = 0
    if prof.is_dir():
        for p in prof.iterdir():
            if p.is_dir():
                shutil.rmtree(p)
            else:
                p.unlink()
            removed += 1
    prof.mkdir(parents=True, exist_ok=True)
    print(f"removed {removed} entries from {prof}")
    return 0


def cmd_report(args: argparse.Namespace) -> int:
    build = Path(args.build_dir).resolve()
    profdata_tool = find_tool("llvm-profdata")
    cov_tool = find_tool("llvm-cov")
    missing = [n for n, t in (("llvm-profdata", profdata_tool), ("llvm-cov", cov_tool)) if not t]
    if missing:
        print(f"coverage_report: not found: {', '.join(missing)} "
              "(macOS: xcode-select --install; elsewhere: the llvm matching the compiler)", file=sys.stderr)
        return 2
    raws = sorted((build / "profiles").glob("*.profraw"))
    if not raws:
        print(f"coverage_report: no *.profraw under {build / 'profiles'}: run ctest --preset coverage first",
              file=sys.stderr)
        return 2
    objects = find_objects(build)
    if not objects:
        print(f"coverage_report: no test executables under {build}/tests or {build}/apps", file=sys.stderr)
        return 2

    merged = build / "coverage.profdata"
    # A response file: thousands of profiles do not fit on one command line.
    listing = build / "profiles.txt"
    listing.write_text("".join(f"{p}\n" for p in raws))
    print(f"merging {len(raws)} profiles", file=sys.stderr)
    r = subprocess.run([profdata_tool, "merge", "-sparse", "-f", str(listing), "-o", str(merged)], check=False)
    if r.returncode != 0:
        print("coverage_report: llvm-profdata merge failed", file=sys.stderr)
        return 2

    cmd = [cov_tool, "export", "-summary-only", f"-instr-profile={merged}", str(objects[0])]
    for o in objects[1:]:
        cmd += ["-object", str(o)]
    cmd.append(str(SOURCE_ROOT / "libs"))
    print(f"llvm-cov export over {len(objects)} objects", file=sys.stderr)
    r = subprocess.run(cmd, capture_output=True, text=True, check=False)
    if r.returncode != 0:
        sys.stderr.write(r.stderr)
        print("coverage_report: llvm-cov export failed", file=sys.stderr)
        return 2
    summary = summarize(json.loads(r.stdout), SOURCE_ROOT)
    Path(args.out).write_text(json.dumps(summary, indent=1) + "\n")
    print(f"libs/ ({len(summary['files'])} files): {format_totals(summary)}")
    return 0


def cmd_compare(args: argparse.Namespace) -> int:
    before = json.loads(Path(args.before).read_text())
    after = json.loads(Path(args.after).read_text())
    print(f"before: {format_totals(before)}")
    print(f"after:  {format_totals(after)}")
    drops: list[str] = []
    for k in KINDS:
        if after["totals"][k]["covered"] < before["totals"][k]["covered"]:
            drops.append(f"  total {k}: {before['totals'][k]['covered']} -> {after['totals'][k]['covered']}")
    for name, row in before["files"].items():
        new = after["files"].get(name)
        for k in KINDS:
            was = row[k]["covered"]
            now = new[k]["covered"] if new else 0
            if now < was:
                gone = "" if new else " (not in the second summary)"
                drops.append(f"  {name}: {k} covered {was} -> {now}{gone}")
    if drops:
        print("covered went down:")
        print("\n".join(drops))
        return 1
    print("no file covers less")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("clean", help="empty <build>/profiles before a run")
    c.add_argument("--build-dir", default="build/coverage")
    c.set_defaults(func=cmd_clean)
    r = sub.add_parser("report", help="merge the profiles and summarize libs/")
    r.add_argument("--build-dir", default="build/coverage")
    r.add_argument("--out", required=True)
    r.set_defaults(func=cmd_report)
    p = sub.add_parser("compare", help="files whose covered lines or branches went down")
    p.add_argument("before")
    p.add_argument("after")
    p.set_defaults(func=cmd_compare)
    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
