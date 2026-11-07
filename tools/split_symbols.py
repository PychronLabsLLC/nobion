#!/usr/bin/env python3
"""Split the debug information of a release build into one archive.

    python3 tools/split_symbols.py <build dir> <out dir> --os macos|linux|windows

Reads `<build>/symbol_files.txt` (or `symbol_files_<config>.txt` from a
multi-config generator; cmake/NobionInstall.cmake writes it): our programs and
shared libraries, and on Windows their PDBs. The version is version.txt's.

  macOS    dsymutil each file; nobion-<version>-macOS-<arch>.dSYM.zip
  Linux    objcopy --only-keep-debug to <name>.debug, then --add-gnu-debuglink
           on the build-tree file (CPack strips the installed copy and the
           debuglink section survives); nobion-<version>-linux-<arch>.debug.tar.zst
  Windows  the PDBs; nobion-<version>-windows-x64.pdb.zip

Never part of an installer. Prints the archive's path.
"""

import argparse
import platform
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def archive_name(version, os_name, arch):
    if os_name == "macos":
        return f"nobion-{version}-macOS-{arch}.dSYM.zip"
    if os_name == "linux":
        return f"nobion-{version}-linux-{arch}.debug.tar.zst"
    return f"nobion-{version}-windows-{arch}.pdb.zip"


def read_list(build, config):
    for name in ("symbol_files.txt", f"symbol_files_{config}.txt"):
        path = Path(build) / name
        if path.exists():
            return [Path(ln.strip()) for ln in path.read_text().splitlines() if ln.strip()]
    raise SystemExit(f"{build}: no symbol_files.txt (configure with cmake first)")


def zip_tree(source, archive):
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as z:
        for path in sorted(Path(source).rglob("*")):
            if path.is_file():
                z.write(path, path.relative_to(source))


def split_macos(files, work, archive):
    for f in files:
        subprocess.run(["dsymutil", str(f), "-o", str(work / (f.name + ".dSYM"))], check=True)
    zip_tree(work, archive)


def split_linux(files, work, archive):
    for f in files:
        debug = work / (f.name + ".debug")
        subprocess.run(["objcopy", "--only-keep-debug", str(f), str(debug)], check=True)
        subprocess.run(["objcopy", f"--add-gnu-debuglink={debug}", str(f)], check=True)
    subprocess.run(["tar", "--zstd", "-cf", str(archive), "-C", str(work), *sorted(p.name for p in work.iterdir())],
                   check=True)


def split_windows(files, work, archive):
    pdbs = [f for f in files if f.suffix.lower() == ".pdb"]
    if not pdbs:
        raise SystemExit("no .pdb in the list")
    for pdb in pdbs:
        shutil.copy2(pdb, work / pdb.name)
    zip_tree(work, archive)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("build", type=Path)
    parser.add_argument("out", type=Path)
    parser.add_argument("--os", choices=["macos", "linux", "windows"], required=True)
    parser.add_argument("--config", default="RelWithDebInfo")
    args = parser.parse_args(argv)

    version = (ROOT / "version.txt").read_text().strip()
    machine = platform.machine().lower()
    arch = {"windows": "x64"}.get(args.os) or ("arm64" if machine in ("arm64", "aarch64") and args.os == "macos"
                                               else machine)
    files = read_list(args.build, args.config)
    missing = [f for f in files if not f.exists()]
    if missing:
        raise SystemExit("not built: " + ", ".join(map(str, missing)))
    args.out.mkdir(parents=True, exist_ok=True)
    archive = (args.out / archive_name(version, args.os, arch)).resolve()
    with tempfile.TemporaryDirectory() as work:
        {"macos": split_macos, "linux": split_linux, "windows": split_windows}[args.os](files, Path(work), archive)
    print(archive)
    return 0


if __name__ == "__main__":
    sys.exit(main())
