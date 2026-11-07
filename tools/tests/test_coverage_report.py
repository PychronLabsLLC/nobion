"""Tests for tools/coverage_report.py (stdlib unittest; run by ctest).

Only what the script decides itself: which files are test binaries, what of
llvm-cov's summary is kept, and what counts as a drop between two summaries.
llvm-profdata and llvm-cov are not run.
"""

import io
import json
import struct
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import coverage_report as cr  # noqa: E402

ROOT = Path("/repo")


def macho(filetype: int) -> bytes:
    # 64-bit little-endian Mach-O header: magic, cputype, cpusubtype, filetype.
    return struct.pack("<IiiI", 0xFEEDFACF, 0x0100000C, 0, filetype) + b"\0" * 16


def stats(count: int, covered: int) -> dict:
    return {"count": count, "covered": covered, "percent": 100.0 * covered / count if count else 0.0}


def export_json(files: dict[str, tuple[int, int, int, int]]) -> dict:
    """llvm-cov export -summary-only output for {path: (lines, covered, branches, covered)}."""
    out = []
    tl = tc = bl = bc = 0
    for name, (lines, lcov, branches, bcov) in files.items():
        out.append({"filename": name, "summary": {"lines": stats(lines, lcov),
                                                  "branches": stats(branches, bcov),
                                                  "functions": stats(1, 1)}})
        tl, tc, bl, bc = tl + lines, tc + lcov, bl + branches, bc + bcov
    return {"data": [{"files": out, "totals": {"lines": stats(tl, tc), "branches": stats(bl, bc)}}],
            "type": "llvm.coverage.json.export", "version": "2.0.1"}


class FindObjects(unittest.TestCase):
    def test_executables_under_tests_and_apps_only(self):
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp)
            files = {
                "tests/core/nobion_core_tests": macho(2),
                "apps/elctl/tests/nobion_elctl_tests": macho(2),
                "apps/elctl/elctl": macho(2),
                "tests/core/libhelper.dylib": macho(6),
                "tests/core/CMakeFiles/a.out": macho(2),
                "tests/core/run.sh": b"#!/bin/sh\n",
                "_deps/gtest/tests/gtest_tests": macho(2),
                "bin/other": macho(2),
            }
            for rel, data in files.items():
                p = build / rel
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_bytes(data)
                p.chmod(0o755)
            found = sorted(p.relative_to(build).as_posix() for p in cr.find_objects(build))
            self.assertEqual(found, ["apps/elctl/elctl", "apps/elctl/tests/nobion_elctl_tests",
                                     "tests/core/nobion_core_tests"])

    def test_elf_executable_is_found_and_shared_object_is_not(self):
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp)
            (build / "tests").mkdir()
            for name in ("t", "libx.so", "libx.so.1"):
                p = build / "tests" / name
                p.write_bytes(b"\x7fELF" + b"\0" * 60)
                p.chmod(0o755)
            found = [p.name for p in cr.find_objects(build)]
            self.assertEqual(found, ["t"])


class Summarize(unittest.TestCase):
    def test_keeps_libs_only_with_paths_relative_to_the_source_tree(self):
        data = export_json({
            "/repo/libs/core/src/a.cpp": (10, 8, 4, 2),
            "/repo/libs/core/include/nobion/core/a.hpp": (5, 5, 0, 0),
            "/repo/tests/core/test_a.cpp": (50, 50, 2, 2),
            "/repo/build/coverage/_deps/x.cpp": (100, 1, 10, 1),
        })
        s = cr.summarize(data, ROOT)
        self.assertEqual(sorted(s["files"]), ["libs/core/include/nobion/core/a.hpp", "libs/core/src/a.cpp"])
        self.assertEqual(s["files"]["libs/core/src/a.cpp"]["lines"], {"count": 10, "covered": 8, "percent": 80.0})
        self.assertEqual(s["totals"]["lines"], {"count": 15, "covered": 13, "percent": 86.67})
        self.assertEqual(s["totals"]["branches"], {"count": 4, "covered": 2, "percent": 50.0})


class Compare(unittest.TestCase):
    def summary(self, files: dict[str, tuple[int, int, int, int]]) -> dict:
        return cr.summarize(export_json({f"/repo/{k}": v for k, v in files.items()}), ROOT)

    def run_compare(self, before: dict, after: dict) -> tuple[int, str]:
        with tempfile.TemporaryDirectory() as tmp:
            b, a = Path(tmp) / "b.json", Path(tmp) / "a.json"
            b.write_text(json.dumps(before))
            a.write_text(json.dumps(after))
            out = io.StringIO()
            with redirect_stdout(out), redirect_stderr(io.StringIO()):
                rc = cr.main(["compare", str(b), str(a)])
            return rc, out.getvalue()

    def test_same_or_more_covered_is_ok(self):
        before = self.summary({"libs/a.cpp": (10, 5, 4, 2)})
        after = self.summary({"libs/a.cpp": (10, 6, 4, 2), "libs/b.cpp": (3, 0, 0, 0)})
        rc, out = self.run_compare(before, after)
        self.assertEqual(rc, 0)
        self.assertIn("lines", out)

    def test_fewer_lines_covered_in_one_file_is_a_drop(self):
        before = self.summary({"libs/a.cpp": (10, 5, 4, 2), "libs/b.cpp": (10, 5, 4, 2)})
        after = self.summary({"libs/a.cpp": (10, 4, 4, 2), "libs/b.cpp": (10, 9, 4, 2)})
        rc, out = self.run_compare(before, after)
        self.assertEqual(rc, 1)
        self.assertIn("libs/a.cpp", out)
        self.assertNotIn("libs/b.cpp", out)

    def test_fewer_branches_covered_is_a_drop(self):
        before = self.summary({"libs/a.cpp": (10, 5, 4, 3)})
        after = self.summary({"libs/a.cpp": (10, 5, 4, 2)})
        rc, out = self.run_compare(before, after)
        self.assertEqual(rc, 1)
        self.assertIn("libs/a.cpp", out)

    def test_file_gone_from_the_report_is_a_drop(self):
        before = self.summary({"libs/a.cpp": (10, 5, 4, 2), "libs/b.cpp": (2, 1, 0, 0)})
        after = self.summary({"libs/a.cpp": (10, 5, 4, 2)})
        rc, out = self.run_compare(before, after)
        self.assertEqual(rc, 1)
        self.assertIn("libs/b.cpp", out)


class Report(unittest.TestCase):
    def test_missing_tool_exits_2_with_the_reason(self):
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp)
            (build / "profiles").mkdir()
            (build / "profiles" / "1.profraw").write_bytes(b"x")
            err = io.StringIO()
            with mock.patch.object(cr, "find_tool", return_value=None), redirect_stderr(err):
                rc = cr.main(["report", "--build-dir", str(build), "--out", str(build / "s.json")])
            self.assertEqual(rc, 2)
            self.assertIn("llvm-profdata", err.getvalue())

    def test_no_profiles_exits_2_with_the_reason(self):
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp)
            err = io.StringIO()
            with mock.patch.object(cr, "find_tool", return_value="/usr/bin/true"), redirect_stderr(err):
                rc = cr.main(["report", "--build-dir", str(build), "--out", str(build / "s.json")])
            self.assertEqual(rc, 2)
            self.assertIn("profraw", err.getvalue())


class Clean(unittest.TestCase):
    def test_empties_profiles_and_leaves_the_rest(self):
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp)
            prof = build / "profiles"
            prof.mkdir()
            (prof / "1.profraw").write_bytes(b"x")
            (build / "keep.txt").write_text("k")
            with redirect_stdout(io.StringIO()):
                rc = cr.main(["clean", "--build-dir", str(build)])
            self.assertEqual(rc, 0)
            self.assertTrue(prof.is_dir())
            self.assertEqual(list(prof.iterdir()), [])
            self.assertTrue((build / "keep.txt").exists())


if __name__ == "__main__":
    unittest.main()
