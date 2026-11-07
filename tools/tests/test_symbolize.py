"""tools/symbolize.py and tools/split_symbols.py: crash file parsing, the call made
per frame, and the archive names. No real atos, addr2line, dsymutil or objcopy
is run: stand-ins on PATH record their arguments and answer from a table.
"""

import os
import stat
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

import split_symbols  # noqa: E402
import symbolize  # noqa: E402

CRASH = textwrap.dedent("""\
    fatal signal 11
    build 0.4.0+35feab69c7f5
    thread crash_helper.main
    0x10221c520
    0x10221b82c
    0x181b03744
    module /opt/nobion/bin/crash_helper 0x1021fc000 0xc0000
    module /usr/lib/libc++.1.dylib 0x181a09000 0x200000
    """)


def stand_in(directory, name, answer):
    """A program that appends its arguments to <name>.calls and prints `answer`."""
    path = Path(directory) / name
    path.write_text(f"#!/bin/sh\necho \"$@\" >> '{directory}/{name}.calls'\ncat <<'EOF'\n{answer}\nEOF\n")
    path.chmod(path.stat().st_mode | stat.S_IXUSR)
    return path


class Parse(unittest.TestCase):
    def test_frames_and_modules(self):
        crash = symbolize.parse_crash(CRASH)
        self.assertEqual(crash.frames, [0x10221C520, 0x10221B82C, 0x181B03744])
        self.assertEqual(crash.modules[0], symbolize.Module("/opt/nobion/bin/crash_helper", 0x1021FC000, 0xC0000))
        self.assertEqual(len(crash.modules), 2)

    def test_a_module_path_with_spaces(self):
        crash = symbolize.parse_crash("0x10\nmodule /Applications/My App/bin/x 0x0 0x20\n")
        self.assertEqual(crash.modules[0].path, "/Applications/My App/bin/x")

    def test_module_of_finds_the_range(self):
        crash = symbolize.parse_crash(CRASH)
        self.assertEqual(crash.module_of(0x10221C520).path, "/opt/nobion/bin/crash_helper")
        self.assertEqual(crash.module_of(0x181B03744).path, "/usr/lib/libc++.1.dylib")
        self.assertIsNone(crash.module_of(0x5))
        self.assertIsNone(crash.module_of(0x1021FC000 + 0xC0000))


class Run(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.bin = self.dir / "bin"
        self.bin.mkdir()
        self.symbols = self.dir / "symbols"
        self.symbols.mkdir()
        self.crash = self.dir / "crash_helper-1.txt"
        self.crash.write_text(CRASH)
        self.env = dict(os.environ, PATH=f"{self.bin}{os.pathsep}{os.environ['PATH']}")

    def tearDown(self):
        self.tmp.cleanup()

    def run_tool(self, *args):
        return subprocess.run([sys.executable, "-I", str(TOOLS / "symbolize.py"), *args], env=self.env,
                              capture_output=True, text=True)

    def calls(self, name):
        return (self.bin / f"{name}.calls").read_text().splitlines()

    def test_macos_one_atos_call_per_frame_with_the_load_address(self):
        stand_in(self.bin, "atos", "main (in crash_helper) (crash_helper.cpp:86)")
        dwarf = self.symbols / "crash_helper.dSYM" / "Contents" / "Resources" / "DWARF"
        dwarf.mkdir(parents=True)
        (dwarf / "crash_helper").write_text("")
        result = self.run_tool(str(self.crash), "--symbols", str(self.symbols), "--os", "macos")
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = self.calls("atos")
        self.assertEqual(len(calls), 3)
        self.assertEqual(calls[0], f"-o {dwarf / 'crash_helper'} -l 0x1021fc000 0x10221c520")
        self.assertIn("-l 0x181a09000 0x181b03744", calls[2])
        lines = result.stdout.splitlines()
        self.assertEqual(lines[0], "#0 main crash_helper.cpp:86")
        self.assertEqual(len(lines), 3)

    def test_linux_offset_into_the_module_and_the_debug_file(self):
        stand_in(self.bin, "addr2line", "main\n/src/tests/core/crash_helper.cpp:86")
        (self.symbols / "crash_helper.debug").write_text("")
        result = self.run_tool(str(self.crash), "--symbols", str(self.symbols), "--os", "linux")
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = self.calls("addr2line")
        self.assertEqual(calls[0], f"-f -C -e {self.symbols / 'crash_helper.debug'} 0x20520")
        self.assertEqual(result.stdout.splitlines()[0], "#0 main /src/tests/core/crash_helper.cpp:86")

    def test_linux_prefers_llvm_symbolizer_when_present(self):
        stand_in(self.bin, "llvm-symbolizer", "main\n/src/crash_helper.cpp:86:3\n")
        stand_in(self.bin, "addr2line", "unused")
        result = self.run_tool(str(self.crash), "--symbols", str(self.symbols), "--os", "linux")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse((self.bin / "addr2line.calls").exists())
        self.assertIn("#0 main /src/crash_helper.cpp:86", result.stdout)

    def test_a_frame_outside_every_module_is_printed_unresolved(self):
        stand_in(self.bin, "addr2line", "main\ncrash_helper.cpp:1")
        self.crash.write_text("0x5\nmodule /x/crash_helper 0x1000 0x100\n")
        result = self.run_tool(str(self.crash), "--symbols", str(self.symbols), "--os", "linux")
        self.assertEqual(result.stdout.strip(), "#0 0x5 (no module)")

    def test_a_dmp_prints_the_windows_steps(self):
        dmp = self.dir / "elctl-1.dmp"
        dmp.write_bytes(b"MDMP")
        result = self.run_tool(str(dmp), "--symbols", str(self.symbols))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("WinDbg", result.stdout)
        self.assertIn("Visual Studio", result.stdout)
        self.assertIn(str(self.symbols), result.stdout)


class Split(unittest.TestCase):
    def test_archive_names(self):
        self.assertEqual(split_symbols.archive_name("0.4.0", "macos", "arm64"), "nobion-0.4.0-macOS-arm64.dSYM.zip")
        self.assertEqual(split_symbols.archive_name("0.4.0", "linux", "x86_64"), "nobion-0.4.0-linux-x86_64.debug.tar.zst")
        self.assertEqual(split_symbols.archive_name("0.4.0", "windows", "x64"), "nobion-0.4.0-windows-x64.pdb.zip")

    def test_the_list_is_read_without_blanks_and_relative_to_the_build(self):
        with tempfile.TemporaryDirectory() as d:
            build = Path(d)
            (build / "symbol_files.txt").write_text("/b/elctl\n\n/b/nobion\n")
            self.assertEqual(split_symbols.read_list(build, "RelWithDebInfo"), [Path("/b/elctl"), Path("/b/nobion")])
            (build / "symbol_files.txt").unlink()
            (build / "symbol_files_RelWithDebInfo.txt").write_text("C:/b/elctl.exe\n")
            self.assertEqual(split_symbols.read_list(build, "RelWithDebInfo"), [Path("C:/b/elctl.exe")])


if __name__ == "__main__":
    unittest.main()
