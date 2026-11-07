#!/usr/bin/env python3
"""Name the frames of a Nobion crash file.

    python3 tools/symbolize.py <crash file> --symbols <dir> [--os macos|linux]

<crash file> is `<log dir>/crash/<stem>-<pid>.txt` (libs/core signal_stack.cpp):
frame lines `0x<hex>`, one per line, the faulting instruction's line
`pc 0x<hex>`, and module lines `module <path> 0x<load hex> 0x<size hex>`. <dir> holds the release's split
symbols (tools/split_symbols.py output, unpacked): `<name>.dSYM` bundles on
macOS, `<name>.debug` files on Linux. A frame is looked up in the module whose
range holds it; with no symbol file for that module the module's own path is
tried (a build tree's binary still has its debug info).

Prints `pc function file:line` for the faulting instruction, then
`#n function file:line` per frame. backtrace() in the handler gives return
addresses (the handler's own frames, the signal trampoline, then the
interrupted code's callers), each the instruction after a call: looked up at
address - 1, so the line is the call's, not the next statement's. The one
exception is a frame equal to the pc: glibc's backtrace() has the faulting
instruction itself after the trampoline, and it is looked up as it is
(macOS's leaves it out; the pc line names it). Exit status 1: no frame
resolved to a file and line (symbols that do not match the crash, say); 2: the
crash file or a tool (atos, addr2line) is missing. A `.dmp` (Windows) is not read here:
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
    pc: int = None

    def module_of(self, address):
        for module in self.modules:
            if module.load <= address < module.load + module.size:
                return module
        return None


_FRAME = re.compile(r"^0x([0-9a-fA-F]+)$")
_PC = re.compile(r"^pc 0x([0-9a-fA-F]+)$")
_MODULE = re.compile(r"^module (.+) 0x([0-9a-fA-F]+) 0x([0-9a-fA-F]+)$")


def parse_crash(text):
    frames, modules, pc = [], [], None
    for line in text.splitlines():
        line = line.strip()
        if m := _FRAME.match(line):
            frames.append(int(m.group(1), 16))
        elif m := _PC.match(line):
            pc = int(m.group(1), 16)
        elif m := _MODULE.match(line):
            modules.append(Module(m.group(1), int(m.group(2), 16), int(m.group(3), 16)))
    return Crash(frames, modules, pc)


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
        loc = m.group("loc")
        return (f"{m.group('fn')} {loc}", True) if loc and ":" in loc else (m.group("fn"), False)
    return text, False


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
        return hex(address), False
    where = _location(lines[1])
    return f"{lines[0]} {where}", not where.startswith("??")


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

    try:
        crash = parse_crash(args.crash_file.read_text(errors="replace"))
    except OSError as e:
        print(f"symbolize: cannot read {args.crash_file}: {e.strerror}", file=sys.stderr)
        return 2
    if not crash.frames:
        print(f"{args.crash_file}: no frame lines", file=sys.stderr)
        return 1
    resolve = symbolize_macos if args.os == "macos" else symbolize_linux
    print("frames are return addresses, looked up at address - 1; pc (and a frame equal to it) as it is")
    # (label, address written, address looked up)
    lookups = [("pc", crash.pc, crash.pc)] if crash.pc is not None else []
    lookups += [(f"#{n}", address, address if address == crash.pc else address - 1)
                for n, address in enumerate(crash.frames)]
    resolved = 0
    for label, address, lookup in lookups:
        module = crash.module_of(address)
        if module is None:
            print(f"{label} {hex(address)} (no module)")
            continue
        try:
            text, ok = resolve(args.symbols, crash, lookup, module)
        except FileNotFoundError as e:
            print(f"symbolize: {e.filename} not found: install it, or run on the platform of the crash",
                  file=sys.stderr)
            return 2
        resolved += ok
        print(f"{label} {text}")
    if not resolved:
        print("symbolize: no frame resolved to a file and line", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
