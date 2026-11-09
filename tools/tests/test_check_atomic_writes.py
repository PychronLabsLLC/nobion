"""Tests for tools/check_atomic_writes.py (stdlib unittest; run by ctest)."""

import io
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import check_atomic_writes as caw  # noqa: E402


def make_tree(root: Path, files: dict, allow: str = "") -> Path:
    for rel, text in files.items():
        p = root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)
    allowlist = root / "tools" / "atomic_writes_allowlist.txt"
    allowlist.parent.mkdir(parents=True, exist_ok=True)
    allowlist.write_text(allow)
    return allowlist


def run(root: Path, allowlist: Path):
    out, err = io.StringIO(), io.StringIO()
    with redirect_stdout(out), redirect_stderr(err):
        status = caw.main(["--root", str(root), "--allowlist", str(allowlist)])
    return status, out.getvalue() + err.getvalue()


class CheckAtomicWrites(unittest.TestCase):
    def test_rename_is_reported_with_line(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            al = make_tree(root, {"libs/x/a.cpp": "int f() {\n  fs::rename(tmp, p, ec);\n}\n"})
            status, text = run(root, al)
            self.assertEqual(status, 1)
            self.assertIn("a.cpp:2", text)

    def test_allowlisted_file_passes(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            al = make_tree(root, {"libs/x/a.cpp": "fs::rename(tmp, p, ec);\n"}, "# why\nlibs/x/a.cpp\n")
            self.assertEqual(run(root, al)[0], 0)

    def test_tests_directory_is_skipped(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            al = make_tree(root, {"apps/x/tests/t.cpp": "rename(a, b);\n"})
            self.assertEqual(run(root, al)[0], 0)

    def test_atomic_file_itself_is_never_reported(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            al = make_tree(root, {"libs/core/src/atomic_file.cpp": "::rename(a, b);\n"})
            self.assertEqual(run(root, al)[0], 0)

    def test_movefileex_is_reported(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            al = make_tree(root, {"libs/x/w.cpp": "::MoveFileExW(a, b, 0);\n"})
            status, text = run(root, al)
            self.assertEqual(status, 1)
            self.assertIn("w.cpp:1", text)

    def test_no_rename_passes(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            al = make_tree(root, {"libs/x/a.cpp": "int rename_count = 0;\n"})
            self.assertEqual(run(root, al)[0], 0)


if __name__ == "__main__":
    unittest.main()
