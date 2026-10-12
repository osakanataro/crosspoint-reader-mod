#!/usr/bin/env python3
"""Cut the power after every SdFat sector write and check the card, without a device.

Usage: python3 test/sdfat_write_order/run_write_order_test.py [path/to/SdFat-2.3.1]
The default is the copy a firmware build fetched, .pio/libdeps/default/SdFat,
patched or not. Only a temporary copy is changed.

The test is built twice: with all patches it must report no unsafe cut point;
with 0004 and 0005 reverted it must report some, which shows that the test
still sees the hazard those patches remove.
"""
import importlib.util
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("patch_sdfat", ROOT / "scripts/patch_sdfat.py")
patch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(patch)

ORDER_PATCHES = ("0004-delete-entry-before-freeing-clusters.patch",
                 "0005-shorten-entry-before-freeing-clusters.patch")


def build_and_run(project, dependency, name):
    stub = project / "stub.h"
    # SdFat without Arduino: PrintBasic names the flash-string type, the LFN code calls millis().
    stub.write_text("class __FlashStringHelper;\n#include <stdint.h>\ninline uint32_t millis() { return 0; }\n")
    src = dependency / "src"
    executable = project / name
    subprocess.run(["c++", "-std=c++17", "-O1", "-w", "-include", str(stub),
                    "-DENABLE_ARDUINO_FEATURES=0", "-DENABLE_ARDUINO_SERIAL=0", "-DENABLE_ARDUINO_STRING=0",
                    "-DUSE_BLOCK_DEVICE_INTERFACE=1", "-DSPI_DRIVER_SELECT=3", "-DSDFAT_FILE_TYPE=1",
                    "-DUSE_SEPARATE_FAT_CACHE=1", "-I" + str(src),
                    str(ROOT / "test/sdfat_write_order/WriteOrderTest.cpp"),
                    *map(str, sorted((src / "FatLib").glob("*.cpp"))),
                    *map(str, sorted((src / "common").glob("*.cpp"))),
                    "-o", str(executable)], check=True)
    return subprocess.run([str(executable)]).returncode


def run(source):
    with tempfile.TemporaryDirectory(prefix="crosspoint-sdfat-order-") as tmp:
        project = Path(tmp)
        shutil.copytree(ROOT / "scripts/sdfat_patches", project / "scripts/sdfat_patches")
        dependency = project / ".pio/libdeps/test/SdFat"
        shutil.copytree(source, dependency)
        patch.apply_patches(project, dependency)

        print("== all patches", flush=True)
        status = build_and_run(project, dependency, "patched")
        if status == 2:
            raise SystemExit("FAIL: an SdFat operation under test failed")
        if status != 0:
            raise SystemExit("FAIL: a power cut can leave an entry naming a free cluster")

        for name in reversed(ORDER_PATCHES):
            subprocess.run(["git", "apply", "--reverse", str(project / "scripts/sdfat_patches" / name)],
                           cwd=dependency, check=True)
        print("== without 0004 and 0005", flush=True)
        status = build_and_run(project, dependency, "unpatched")
        if status == 2:
            raise SystemExit("FAIL: an SdFat operation under test failed without 0004 and 0005")
        if status != 1:
            raise SystemExit("FAIL: the test no longer sees the hazard 0004 and 0005 remove")
    print("PASS: no unsafe power-cut point with the patches; the test still detects one without them")


if __name__ == "__main__":
    default = ROOT / ".pio/libdeps/default/SdFat"
    run(Path(sys.argv[1] if len(sys.argv) > 1 else default).resolve())
