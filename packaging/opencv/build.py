#!/usr/bin/env python3
"""Build the trimmed, static OpenCV a release links (spec 2026-11-06-field-diagnostics, 12.1).

    python3 packaging/opencv/build.py <src> <prefix>

<src> is a working directory: the pinned tarball is downloaded into it (or
taken from it when already there), checked against its sha256, unpacked, and
built in <src>/build. The result is installed into <prefix>; configure nobion
with -DOpenCV_DIR=<prefix>/<cmake dir> (--print-cmake-dir) and
-DNOBION_VISION_OPENCV=ON.

Only core, imgproc and videoio (and imgcodecs, which videoio needs) are built,
static, with the camera backend of the OS and nothing that brings a shared
library of its own: the package has nothing new to bundle, sign or depend on.
After the install the exported link interface is read, and a library outside
the prefix that is not the system's fails the build.

release.yml keys its cache on this file's sha256: a change to an option here
is a new build.

    --print-cmake-args --os macos|linux|windows [--arch A]   the option set, one -D per line
    --print-cmake-dir --os macos|linux|windows               where OpenCVConfig.cmake lands, from <prefix>
    --print-version                                          the pinned OpenCV version
"""

from __future__ import annotations

import argparse
import hashlib
import os
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

OPENCV_VERSION = "4.14.0"
OPENCV_URL = f"https://github.com/opencv/opencv/archive/refs/tags/{OPENCV_VERSION}.tar.gz"
# sha256 of the archive above, computed from the downloaded file on 2026-11-07.
OPENCV_SHA256 = "ee8fb9b30eb60850431b4656447080e3737b56e45719c92b67f245950609f86e"

COMMON = [
    ("BUILD_LIST", "core,imgproc,videoio"),
    ("BUILD_SHARED_LIBS", "OFF"),
    ("BUILD_WITH_STATIC_CRT", "OFF"),  # MSVC /MD, as nobion
    ("CMAKE_POSITION_INDEPENDENT_CODE", "ON"),
    ("CMAKE_BUILD_TYPE", "Release"),
    ("BUILD_ZLIB", "ON"),  # bundled zlib, static
    # JPEG stays, from OpenCV's bundled libjpeg-turbo (static): OpenCV's own
    # MJPEG reader (the only video file reader on Linux without FFmpeg) and a
    # V4L camera sending MJPEG decode through it.
    ("WITH_JPEG", "ON"),
    ("BUILD_JPEG", "ON"),
]

# Each of these, left to OpenCV's default, would build or link something the
# code does not use, and some (AVIF, Orbbec SDK, VA) a shared library found on
# the build machine.
OFF = [
    "WITH_FFMPEG", "WITH_GSTREAMER", "WITH_TBB", "WITH_OPENMP", "WITH_OPENCL",
    "WITH_IPP", "WITH_ITT", "WITH_PROTOBUF", "WITH_EIGEN", "WITH_LAPACK",
    # image codecs: imgcodecs is built only because videoio needs it
    "WITH_PNG", "WITH_TIFF", "WITH_WEBP", "WITH_OPENJPEG",
    "WITH_OPENEXR", "WITH_JASPER", "WITH_AVIF",
    "WITH_IMGCODEC_GIF", "WITH_IMGCODEC_HDR", "WITH_IMGCODEC_SUNRASTER",
    "WITH_IMGCODEC_PXM", "WITH_IMGCODEC_PFM",
    # Orbbec depth cameras (the SDK is a downloaded shared library on macOS);
    # VA-API (libva, Linux); VTK (found in Homebrew, it brings OpenGL into
    # opencv_core's link interface); KleidiCV (arm64: a second download, from
    # gitlab.arm.com, at configure time); ADE (G-API's, downloaded at
    # configure time though G-API is not built)
    "WITH_OBSENSOR", "WITH_VA", "WITH_VA_INTEL", "WITH_VTK", "WITH_KLEIDICV", "WITH_ADE",
    "BUILD_TESTS", "BUILD_PERF_TESTS", "BUILD_EXAMPLES", "BUILD_DOCS",
    "BUILD_opencv_apps", "BUILD_JAVA", "BUILD_opencv_python3",
]

BACKENDS = {
    "macos": ["WITH_AVFOUNDATION"],
    "linux": ["WITH_V4L"],
    "windows": ["WITH_MSMF", "WITH_DSHOW"],
}

CMAKE_DIR = {"macos": "lib/cmake/opencv4", "linux": "lib/cmake/opencv4", "windows": "."}


def host_os() -> str:
    if sys.platform == "darwin":
        return "macos"
    if sys.platform == "win32":
        return "windows"
    return "linux"


def cmake_args(os_name: str, arch: str | None = None) -> list[str]:
    """The -D options for os_name, in a fixed order."""
    opts = list(COMMON)
    opts += [(name, "ON") for name in BACKENDS[os_name]]
    opts += [(name, "OFF") for name in OFF]
    if os_name == "macos":
        # As the app's: nobion sets neither, so both builds take the
        # environment's deployment target and the runner's architecture.
        opts.append(("CMAKE_OSX_ARCHITECTURES", arch or platform.machine()))
        target = os.environ.get("MACOSX_DEPLOYMENT_TARGET")
        if target:
            opts.append(("CMAKE_OSX_DEPLOYMENT_TARGET", target))
    return [f"-D{name}={value}" for name, value in opts]


def sha256_matches(path: Path, expected: str) -> bool:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest() == expected


_LINK_LIBS = re.compile(r'INTERFACE_LINK_LIBRARIES\s+"([^"]*)"')
_SYSTEM_PATH = re.compile(r"\.framework$|/SDKs/|Windows Kits", re.IGNORECASE)


def foreign_libraries(text: str) -> list[str]:
    """Absolute library paths in an exported OpenCVModules*.cmake that are not the system's.

    Libraries OpenCV installed itself are written relative to ${_IMPORT_PREFIX};
    system ones are bare names (dl, m, pthread, mfplat) or -framework flags.
    """
    found = []
    for value in _LINK_LIBS.findall(text):
        for item in value.split(";"):
            item = item.replace("\\$<LINK_ONLY:", "").replace("$<LINK_ONLY:", "").rstrip(">").strip()
            absolute = item.startswith("/") or re.match(r"^[A-Za-z]:[/\\]", item)
            if absolute and not _SYSTEM_PATH.search(item):
                found.append(item)
    return found


def run(cmd: list[str]) -> None:
    print("+", " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True)


def fetch(src: Path) -> Path:
    tarball = src / f"opencv-{OPENCV_VERSION}.tar.gz"
    if not (tarball.exists() and sha256_matches(tarball, OPENCV_SHA256)):
        print(f"downloading {OPENCV_URL}", flush=True)
        partial = tarball.with_suffix(".partial")
        urllib.request.urlretrieve(OPENCV_URL, partial)
        if not sha256_matches(partial, OPENCV_SHA256):
            partial.unlink()
            sys.exit(f"error: {OPENCV_URL} does not have sha256 {OPENCV_SHA256}")
        partial.replace(tarball)
    source = src / f"opencv-{OPENCV_VERSION}"
    if not (source / "CMakeLists.txt").exists():
        with tarfile.open(tarball) as tar:
            if hasattr(tarfile, "data_filter"):
                tar.extractall(src, filter="data")
            else:  # Python before 3.12: check the names by hand
                for member in tar.getmembers():
                    if member.name.startswith(("/", "..")) or "/../" in member.name:
                        sys.exit(f"error: {member.name} leaves the source directory")
                tar.extractall(src)  # noqa: S202 (names checked above)
    return source


def build(src: Path, prefix: Path) -> None:
    os_name = host_os()
    src.mkdir(parents=True, exist_ok=True)
    source = fetch(src)
    build_dir = src / "build"
    gen = []
    if os_name == "windows":
        gen = ["-G", "Visual Studio 17 2022", "-A", "x64"]
    elif shutil.which("ninja"):
        gen = ["-G", "Ninja"]
    run(["cmake", "-S", str(source), "-B", str(build_dir), *gen,
         f"-DCMAKE_INSTALL_PREFIX={prefix}", *cmake_args(os_name)])
    run(["cmake", "--build", str(build_dir), "--config", "Release", "--parallel", str(os.cpu_count() or 2)])
    run(["cmake", "--install", str(build_dir), "--config", "Release", "--prefix", str(prefix)])
    check(prefix, os_name)


def check(prefix: Path, os_name: str) -> None:
    """Fail when the install is not what a package can link as it is."""
    config = prefix / CMAKE_DIR[os_name] / "OpenCVConfig.cmake"
    if not config.exists():
        sys.exit(f"error: no {config}")
    problems = []
    for modules in prefix.rglob("OpenCVModules*.cmake"):
        problems += [f"{modules.name}: {lib}" for lib in foreign_libraries(modules.read_text())]
    shared = [p for p in prefix.rglob("*") if p.is_file() and re.search(r"\.(dylib|dll)$|\.so(\.\d+)*$", p.name)]
    problems += [f"shared library installed: {p}" for p in shared]
    if problems:
        sys.exit("error: the OpenCV install links outside itself:\n  " + "\n  ".join(problems))
    print(f"OpenCV {OPENCV_VERSION} installed in {prefix}; OpenCV_DIR={prefix / CMAKE_DIR[os_name]}")


def main(argv: list[str]) -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("src", nargs="?", type=Path)
    p.add_argument("prefix", nargs="?", type=Path)
    p.add_argument("--print-cmake-args", action="store_true")
    p.add_argument("--print-cmake-dir", action="store_true")
    p.add_argument("--print-version", action="store_true")
    p.add_argument("--os", choices=sorted(BACKENDS), default=host_os())
    p.add_argument("--arch", help="macOS architecture (default: this machine's)")
    a = p.parse_args(argv)
    if a.print_cmake_args:
        print("\n".join(cmake_args(a.os, a.arch)))
        return 0
    if a.print_cmake_dir:
        print(CMAKE_DIR[a.os])
        return 0
    if a.print_version:
        print(OPENCV_VERSION)
        return 0
    if a.src is None or a.prefix is None:
        p.error("give <src> and <prefix>")
    build(a.src.resolve(), a.prefix.resolve())
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
