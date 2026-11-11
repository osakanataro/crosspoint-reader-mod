#!/usr/bin/env python3
"""Build the UI language packs published on crosspoint-assets.

Every language except English becomes <code>.cplang (lowercase code), the file
LanguagePackDownloadActivity fetches. Packs are matched to the firmware by key
name, so one set serves every firmware that reads its pack format.
"""

from __future__ import annotations

import argparse
import hashlib
import subprocess
import sys
import tempfile
from pathlib import Path


def build_assets(firmware: Path, output: Path) -> None:
    output.mkdir(parents=True, exist_ok=True)
    for old in output.glob("*.cplang"):
        old.unlink()
    with tempfile.TemporaryDirectory() as generated:
        # The C++ output is not needed; it goes to a scratch directory.
        subprocess.run(
            [
                sys.executable,
                str(firmware / "scripts" / "gen_i18n.py"),
                str(firmware / "lib" / "I18n" / "translations"),
                generated,
                "--builtin-langs",
                "en",
                "--pack-dir",
                str(output),
            ],
            cwd=firmware,
            check=True,
        )

    packs = sorted(output.glob("*.cplang"))
    if not packs:
        raise ValueError("gen_i18n.py wrote no packs")
    hashes = [f"{hashlib.sha256(pack.read_bytes()).hexdigest()}  {pack.name}" for pack in packs]
    (output / "SHA256SUMS-lang").write_text("\n".join(hashes) + "\n")
    print(f"Built {len(packs)} language packs")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--firmware", type=Path, default=Path(__file__).resolve().parent.parent,
                        help="Firmware checkout (default: this one)")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    build_assets(args.firmware, args.output)
