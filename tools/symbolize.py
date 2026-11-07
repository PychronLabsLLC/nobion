#!/usr/bin/env python3
"""Name the frames of a Nobion crash file.

    python3 tools/symbolize.py <crash file> --symbols <dir> [--os macos|linux]

<crash file> is `<log dir>/crash/<stem>-<pid>.txt` (libs/core signal_stack.cpp):
frame lines `0x<hex>`, one per line, and module lines
`module <path> 0x<load hex> 0x<size hex>`. <dir> holds the release's split
symbols (tools/split_symbols.py output, unpacked): `<name>.dSYM` bundles on
macOS, `<name>.debug` files on Linux. A frame is looked up in the module whose
range holds it; with no symbol file for that module the module's own path is
tried (a build tree's binary still has its debug info).

Prints `#n function file:line` per frame. A `.dmp` (Windows) is not read here:
the steps for WinDbg and Visual Studio are printed instead.
"""

import argparse
import platform
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Module:
    path: str
    load: int
    size: int


@dataclass
class Crash:
    frames: list
    modules: list

    def module_of(self, address):
        for module in self.modules:
            if module.load <= address < module.load + module.size:
                return module
        return None


_FRAME = re.compile(r"^0x([0-9a-fA-F]+)$")
_MODULE = re.compile(r"^module (.+) 0x([0-9a-fA-F]+) 0x([0-9a-fA-F]+)$")


def parse_crash(text):
    frames, modules = [], []
    for line in text.splitlines():
        line = line.strip()
        if m := _FRAME.match(line):
            frames.append(int(m.group(1), 16))
        elif m := _MODULE.match(line):
            modules.append(Module(m.group(1), int(m.group(2), 16), int(m.group(3), 16)))
    return Crash(frames, modules)


def find_macos_symbols(symbols, module):
    """The DWARF file inside <name>.dSYM, else the module's own file."""
    name = Path(module.path).name
    for bundle in sorted(Path(symbols).rglob(f"{name}.dSYM")):
        inner = bundle / "Contents" / "Resources" / "DWARF" / name
        if inner.exists():
            return inner
    return Path(module.path)


def find_linux_symbols(symbols, module):
    name = Path(module.path).name
    for candidate in (Path(symbols) / f"{name}.debug", *sorted(Path(symbols).rglob(f"{name}.debug"))):
        if candidate.exists():
            return candidate
    return Path(module.path)


_ATOS = re.compile(r"^(?P<fn>.+?) \(in [^)]*\)(?: \((?P<loc>[^)]*)\))?$")


def symbolize_macos(symbols, crash, address, module):
    target = find_macos_symbols(symbols, module)
    out = subprocess.run(["atos", "-o", str(target), "-l", hex(module.load), hex(address)],
                         capture_output=True, text=True, check=False)
    text = out.stdout.strip().splitlines()[0] if out.stdout.strip() else hex(address)
    if m := _ATOS.match(text):
        return f"{m.group('fn')} {m.group('loc')}" if m.group("loc") else m.group("fn")
    return text


def _location(text):
    """`file:line` or `file:line:column` becomes `file:line`; `??:0` stays."""
    m = re.match(r"^(.*?):(\d+)(?::\d+)?(?: .*)?$", text)
    return f"{m.group(1)}:{m.group(2)}" if m else text


def symbolize_linux(symbols, crash, address, module):
    target = find_linux_symbols(symbols, module)
    offset = hex(address - module.load)
    if shutil.which("llvm-symbolizer"):
        cmd = ["llvm-symbolizer", f"--obj={target}", "--functions", "--demangle", offset]
    else:
        cmd = ["addr2line", "-f", "-C", "-e", str(target), offset]
    out = subprocess.run(cmd, capture_output=True, text=True, check=False)
    lines = [ln for ln in out.stdout.splitlines() if ln.strip()]
    if len(lines) < 2:
        return hex(address)
    return f"{lines[0]} {_location(lines[1])}"


WINDOWS_STEPS = """\
{dmp} is a Windows minidump. It is not read here. With the release's
symbols ({symbols}, the PDBs unpacked from nobion-<version>-windows-x64.pdb.zip):

WinDbg
  1. File > Open dump file: {dmp}
  2. .sympath {symbols}
  3. .reload /f
  4. !analyze -v, then k for the stack of the crashing thread.

Visual Studio
  1. File > Open > File: {dmp}
  2. Debug with Native Only.
  3. Tools > Options > Debugging > Symbols: add {symbols}.
  4. Open the Call Stack window.
"""


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("crash_file", type=Path)
    parser.add_argument("--symbols", type=Path, required=True)
    parser.add_argument("--os", choices=["macos", "linux"],
                        default="macos" if platform.system() == "Darwin" else "linux")
    args = parser.parse_args(argv)

    if args.crash_file.suffix.lower() == ".dmp":
        print(WINDOWS_STEPS.format(dmp=args.crash_file, symbols=args.symbols))
        return 0

    crash = parse_crash(args.crash_file.read_text(errors="replace"))
    if not crash.frames:
        print(f"{args.crash_file}: no frame lines", file=sys.stderr)
        return 1
    resolve = symbolize_macos if args.os == "macos" else symbolize_linux
    for n, address in enumerate(crash.frames):
        module = crash.module_of(address)
        if module is None:
            print(f"#{n} {hex(address)} (no module)")
        else:
            print(f"#{n} {resolve(args.symbols, crash, address, module)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
