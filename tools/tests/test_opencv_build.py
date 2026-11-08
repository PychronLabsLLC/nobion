"""Tests for packaging/opencv/build.py (stdlib unittest; run by ctest).

The option set a release's OpenCV is built with is pinned here, one table per
OS, as spec docs/superpowers/specs/2026-11-06-field-diagnostics-design.md
section 12.1 lists it. No OpenCV is downloaded or built.
"""

from __future__ import annotations

import hashlib
import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "packaging" / "opencv" / "build.py"

sys.path.insert(0, str(SCRIPT.parent))
import build as ocv  # noqa: E402

COMMON = {
    "BUILD_LIST": "core,imgproc,videoio",
    "BUILD_SHARED_LIBS": "OFF",
    "BUILD_WITH_STATIC_CRT": "OFF",
    "CMAKE_POSITION_INDEPENDENT_CODE": "ON",
    "CMAKE_BUILD_TYPE": "Release",
    "BUILD_ZLIB": "ON",
    # Amendment of 2026-11-07: OpenCV's MJPEG reader and V4L's MJPEG cameras need it.
    "WITH_JPEG": "ON",
    "BUILD_JPEG": "ON",
}

# Spec 12.1's "Off" list, and the hidden dependencies the amendment of
# 2026-11-07 adds (WITH_AVIF, WITH_OBSENSOR, WITH_VA, WITH_VA_INTEL,
# WITH_VTK, WITH_KLEIDICV, WITH_ADE).
OFF = [
    "WITH_FFMPEG", "WITH_GSTREAMER", "WITH_TBB", "WITH_OPENMP", "WITH_OPENCL",
    "WITH_IPP", "WITH_ITT", "WITH_PROTOBUF", "WITH_EIGEN", "WITH_LAPACK",
    "WITH_PNG", "WITH_TIFF", "WITH_WEBP", "WITH_OPENJPEG",
    "WITH_OPENEXR", "WITH_JASPER", "WITH_AVIF",
    "WITH_IMGCODEC_GIF", "WITH_IMGCODEC_HDR", "WITH_IMGCODEC_SUNRASTER",
    "WITH_IMGCODEC_PXM", "WITH_IMGCODEC_PFM",
    "WITH_OBSENSOR", "WITH_VA", "WITH_VA_INTEL", "WITH_VTK", "WITH_KLEIDICV", "WITH_ADE",
    "BUILD_TESTS", "BUILD_PERF_TESTS", "BUILD_EXAMPLES", "BUILD_DOCS",
    "BUILD_opencv_apps", "BUILD_JAVA", "BUILD_opencv_python3",
]

BACKENDS = {
    "macos": ["WITH_AVFOUNDATION"],
    "linux": ["WITH_V4L"],
    "windows": ["WITH_MSMF", "WITH_DSHOW"],
}


def expected(os_name: str, extra: dict | None = None) -> dict:
    want = dict(COMMON)
    want.update({name: "OFF" for name in OFF})
    want.update({name: "ON" for name in BACKENDS[os_name]})
    want.update(extra or {})
    return want


def run(*args: str, env: dict | None = None) -> str:
    base = {k: v for k, v in os.environ.items() if k not in ("MACOSX_DEPLOYMENT_TARGET",)}
    base.update(env or {})
    out = subprocess.run([sys.executable, "-I", str(SCRIPT), *args], capture_output=True, text=True,
                         env=base, check=True)
    return out.stdout


def parse(text: str) -> dict:
    lines = [line for line in text.splitlines() if line]
    got = {}
    for line in lines:
        m = re.fullmatch(r"-D([A-Za-z0-9_]+)=(.*)", line)
        assert m, f"not a -DNAME=VALUE line: {line!r}"
        assert m.group(1) not in got, f"{m.group(1)} given twice"
        got[m.group(1)] = m.group(2)
    return got


class PrintCmakeArgs(unittest.TestCase):
    def test_linux(self):
        self.assertEqual(parse(run("--print-cmake-args", "--os", "linux")), expected("linux"))

    def test_windows(self):
        self.assertEqual(parse(run("--print-cmake-args", "--os", "windows")), expected("windows"))

    def test_macos_architecture(self):
        got = parse(run("--print-cmake-args", "--os", "macos", "--arch", "arm64"))
        self.assertEqual(got, expected("macos", {"CMAKE_OSX_ARCHITECTURES": "arm64"}))

    def test_macos_deployment_target_follows_the_environment(self):
        # The app sets no deployment target of its own: both builds take the
        # environment's (the compiler's default when unset).
        got = parse(run("--print-cmake-args", "--os", "macos", "--arch", "arm64",
                        env={"MACOSX_DEPLOYMENT_TARGET": "13.0"}))
        self.assertEqual(got, expected("macos", {"CMAKE_OSX_ARCHITECTURES": "arm64",
                                                 "CMAKE_OSX_DEPLOYMENT_TARGET": "13.0"}))

    def test_every_listed_option_is_off(self):
        for os_name in BACKENDS:
            got = parse(run("--print-cmake-args", "--os", os_name, "--arch", "arm64"))
            for name in OFF:
                self.assertEqual(got.get(name), "OFF", f"{os_name}: {name}")
            for other, names in BACKENDS.items():
                for name in names:
                    if other != os_name:
                        self.assertNotIn(name, got, f"{os_name}: {name}")

    def test_cmake_dir(self):
        self.assertEqual(run("--print-cmake-dir", "--os", "macos").strip(), "lib/cmake/opencv4")
        self.assertEqual(run("--print-cmake-dir", "--os", "linux").strip(), "lib/cmake/opencv4")
        # OpenCV's Windows layout puts OpenCVConfig.cmake at the prefix's root.
        self.assertEqual(run("--print-cmake-dir", "--os", "windows").strip(), ".")


class Pin(unittest.TestCase):
    def test_version_is_a_4x_release(self):
        self.assertRegex(ocv.OPENCV_VERSION, r"^4\.\d+\.\d+$")
        self.assertEqual(run("--print-version").strip(), ocv.OPENCV_VERSION)

    def test_sha256_is_64_hex(self):
        self.assertRegex(ocv.OPENCV_SHA256, r"^[0-9a-f]{64}$")

    def test_url_is_the_official_archive(self):
        self.assertEqual(ocv.OPENCV_URL,
                         f"https://github.com/opencv/opencv/archive/refs/tags/{ocv.OPENCV_VERSION}.tar.gz")

    def test_sha256_check(self):
        with tempfile.TemporaryDirectory() as tmp:
            f = Path(tmp) / "a.tar.gz"
            f.write_bytes(b"not opencv")
            self.assertTrue(ocv.sha256_matches(f, hashlib.sha256(b"not opencv").hexdigest()))
            self.assertFalse(ocv.sha256_matches(f, ocv.OPENCV_SHA256))


class ForeignLibraries(unittest.TestCase):
    """What the installed config hands a linker: nothing outside the prefix but system libraries."""

    def test_system_and_prefix_libraries_pass(self):
        text = ('set_target_properties(opencv_core PROPERTIES\n'
                '  INTERFACE_LINK_LIBRARIES "\\$<LINK_ONLY:zlib>;\\$<LINK_ONLY:dl>;\\$<LINK_ONLY:m>;'
                '\\$<LINK_ONLY:-framework AVFoundation>;\\$<LINK_ONLY:${_IMPORT_PREFIX}/lib/opencv4/3rdparty/liblibzlib.a>"\n)\n')
        self.assertEqual(ocv.foreign_libraries(text), [])

    def test_a_library_outside_the_prefix_is_reported(self):
        text = ('  INTERFACE_LINK_LIBRARIES "opencv_core;'
                '\\$<LINK_ONLY:/opt/homebrew/lib/libavif.dylib>;\\$<LINK_ONLY:/usr/lib/x86_64-linux-gnu/libva.so>"\n')
        self.assertEqual(ocv.foreign_libraries(text),
                         ["/opt/homebrew/lib/libavif.dylib", "/usr/lib/x86_64-linux-gnu/libva.so"])


if __name__ == "__main__":
    unittest.main()
