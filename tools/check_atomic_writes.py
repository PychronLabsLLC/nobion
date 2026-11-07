#!/usr/bin/env python3
"""Refuses a hand-made temp-file-and-rename write in libs/ and apps/.

State the process reads back after a restart is written with
nobion::write_file_atomic (libs/core atomic_file.hpp), which syncs the file and
the directory. A `rename(` in a .cpp file elsewhere is a writer that does not,
unless the file is listed in tools/atomic_writes_allowlist.txt (one path per
line, relative to the repository root; '#' starts a comment).

Paths with a `tests` directory component and atomic_file.cpp are skipped.
Exit status: 0 clean, 1 findings.
"""

import argparse
import re
import sys
from pathlib import Path

RENAME = re.compile(r"\brename\s*\(")
SKIP_FILES = {"libs/core/src/atomic_file.cpp"}


def load_allowlist(path: Path) -> set:
    if not path.is_file():
        return set()
    entries = set()
    for line in path.read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            entries.add(line)
    return entries


def findings(root: Path, allowed: set):
    for top in ("libs", "apps"):
        base = root / top
        if not base.is_dir():
            continue
        for p in sorted(base.rglob("*.cpp")):
            rel = p.relative_to(root)
            if "tests" in rel.parts[:-1]:
                continue
            key = rel.as_posix()
            if key in SKIP_FILES or key in allowed:
                continue
            for number, line in enumerate(p.read_text(errors="replace").splitlines(), 1):
                code = line.split("//", 1)[0]
                if RENAME.search(code):
                    yield key, number, line.strip()


def main(argv=None) -> int:
    here = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=here.parent)
    parser.add_argument("--allowlist", type=Path, default=here / "atomic_writes_allowlist.txt")
    args = parser.parse_args(argv)
    found = list(findings(args.root, load_allowlist(args.allowlist)))
    for key, number, text in found:
        print(f"{key}:{number}: rename without write_file_atomic: {text}", file=sys.stderr)
    if found:
        print("check_atomic_writes: use nobion::write_file_atomic (libs/core/atomic_file.hpp), or list the "
              "file in tools/atomic_writes_allowlist.txt with the reason.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
