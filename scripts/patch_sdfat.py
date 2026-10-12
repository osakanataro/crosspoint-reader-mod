"""
PlatformIO post: script: apply CrossPoint's SdFat patches via `git apply`.

The patches in `scripts/sdfat_patches/` make USE_SEPARATE_FAT_CACHE
overridable, invalidate the sector cache after a failed read, and sync the
separate FAT cache before the directory sector, and rewrite a directory entry
before freeing the clusters it named (see the file headers). They target SdFat 2.3.1 only.

Unlike JPEGDEC, SdFat is a registry dependency of the SDK, so the script
patches only the copy that the current environment actually builds:
  * the target must live under this project's `.pio/libdeps/<env>/`
  * `library.properties` must say version 2.3.1
  * each target file must hash to the reviewed upstream or patched bytes
Any mismatch fails the build instead of compiling an unreviewed cache.
Upgrading SdFat therefore requires re-reviewing the patches and hashes.

All patches are checked before any file is changed, and already-patched
files are left untouched so incremental builds do not recompile SdFat.
"""

import hashlib
import os
from pathlib import Path
import subprocess


# Reviewed upstream and patched bytes. A dependency upgrade requires re-review.
PATCHES = (
    ("0001-invalidate-failed-cache-fill.patch", "src/common/FsCache.cpp",
     "ba4f99dd660c7c6a747b20abcf11354379aa689115d1ee1ad1cf9577706a67bb",
     "f46a551f97c674ab00c9c25385af7c370a776725f17e14502fcc357858f583da"),
    ("0002-allow-separate-fat-cache-override.patch", "src/SdFatConfig.h",
     "7889975cad262158e1623c873730210c41c55c2198830d668952b82e588ff7cc",
     "32104db82acc857b70c7fe20740afbffa9f7dee0a7c685c106febfa989fdbe77"),
    # 0003 is not upstream's: it comes from witchhunt-reader (jpirnay) and puts the FAT
    # before the directory sector in cacheSync(), see the patch header.
    ("0003-sync-fat-cache-before-data-cache.patch", "src/FatLib/FatPartition.h",
     "fb91c81beac2779a0d4b08e886a5fefdf8062b8ad422ccec4e178ae4e88b72e5",
     "ad9d6a745aa8c91c8f006a61b2a6296e8dd89a1d05f1b335446e371ec541efcc"),
    # 0004 and 0005 are this fork's: with 0003's order, freeing clusters must wait
    # until the entry no longer names them, see the patch headers.
    ("0004-delete-entry-before-freeing-clusters.patch", "src/FatLib/FatFileLFN.cpp",
     "e7a13328625505c5f2c18279087a36d90b308bc7c97513371a6ce14884026be2",
     "84b9fe6bfae78ceaa02fa76e3cd2b946dbbd7b323b53e148263997db6f0829cf"),
    ("0005-shorten-entry-before-freeing-clusters.patch", "src/FatLib/FatFile.cpp",
     "c1757a57ea134df2aab650d2db109b7d731f66cf309e5a070a9b50667c258948",
     "31a94e51f3b7dac555ed7a9b98277ad613f0c4fb84daf6f21c1769cecdb1d3ae"),
)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def is_within(path, root):
    # Path.is_relative_to() needs Python 3.9; the documented minimum is 3.8.
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def apply_patches(project_dir, dependency_dir):
    project = Path(project_dir).resolve()
    dependency = Path(dependency_dir).resolve()
    local_root = project / ".pio" / "libdeps"
    if not is_within(dependency, local_root):
        raise RuntimeError("SdFat must be inside this project's .pio/libdeps")
    properties = dependency / "library.properties"
    targets = [properties] + [dependency / patch[1] for patch in PATCHES]
    for target in targets:
        if not is_within(target.resolve(), dependency) or not target.is_file():
            raise RuntimeError("Missing or escaping SdFat patch target: " + target.name)
    if "version=2.3.1" not in properties.read_text().splitlines():
        raise RuntimeError("SdFat patches require version 2.3.1")

    # Archive dependencies must not discover the enclosing application Git repo.
    process_env = {key: os.environ[key] for key in ("HOME", "PATH", "TMPDIR", "LANG") if key in os.environ}
    process_env.update(GIT_CEILING_DIRECTORIES=str(dependency.parent),
                       GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM="1")
    pending = []
    for name, relative, upstream, patched in PATCHES:
        target = dependency / relative
        current = digest(target)
        if current not in (upstream, patched):
            raise RuntimeError("Unrecognized SdFat source: " + relative)
        patch = project / "scripts" / "sdfat_patches" / name
        # Validate the entire set before changing any dependency files.
        command = ["git", "apply", "--check"]
        if current == patched:
            command.append("--reverse")
        result = subprocess.run(command + [str(patch)], cwd=dependency,
                                env=process_env, capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError("SdFat patch does not apply: " + name + "\n" + result.stderr)
        if current == upstream:
            pending.append((patch, target, patched))
    for patch, target, expected in pending:
        subprocess.run(["git", "apply", str(patch)], cwd=dependency, env=process_env, check=True)
        if digest(target) != expected:
            raise RuntimeError("Unexpected patched SdFat source: " + target.name)
        print("Applied SdFat patch: " + patch.name)


def patch_selected_dependency(env):
    # PioArduino's SDK-only pass clears lib_deps; the later app pass resolves them.
    if env.get("ARDUINO_LIB_COMPILE_FLAG") == "Build":
        return
    # A post: script sees the resolved library builders on the first build too.
    selected = [builder for builder in env.GetLibBuilders()
                if builder.name == "SdFat" and builder.is_dependent]
    if len(selected) != 1:
        raise RuntimeError("Expected exactly one selected SdFat dependency")
    dependency = Path(selected[0].path).resolve()
    environment_root = Path(env.subst("$PROJECT_LIBDEPS_DIR")) / env["PIOENV"]
    if not is_within(dependency, environment_root.resolve()):
        raise RuntimeError("SdFat is outside the selected environment's dependencies")
    apply_patches(env["PROJECT_DIR"], dependency)


if "Import" in globals():
    Import("env")  # noqa: F821 -- supplied by SCons
    patch_selected_dependency(env)  # noqa: F821
