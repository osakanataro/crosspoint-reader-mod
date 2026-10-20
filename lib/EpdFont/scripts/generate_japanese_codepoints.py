#!/usr/bin/env python3
"""Generate codepoints/japanese_jis0213.txt for --codepoints-file.

Lists kana, common punctuation and every ideograph that Unihan maps to
JIS X 0208, 0212 or 0213. Based on the script in zrn-ns/crosspoint-jp.

Usage: python3 generate_japanese_codepoints.py [path/to/Unihan.zip]
Without an argument, Unihan.zip is downloaded from unicode.org.
"""

import io
import sys
import urllib.request
import zipfile
from datetime import date
from pathlib import Path

UNIHAN_ZIP_URL = "https://www.unicode.org/Public/17.0.0/ucd/Unihan.zip"
OUTPUT_PATH = Path(__file__).resolve().parent / "codepoints" / "japanese_jis0213.txt"

FIXED_RANGES = [
    (0x0020, 0x007E),  # ASCII printable
    (0x0080, 0x00FF),  # Latin-1 Supplement
    (0x2000, 0x206F),  # General Punctuation
    (0x3000, 0x303F),  # CJK Symbols and Punctuation
    (0x3040, 0x309F),  # Hiragana
    (0x30A0, 0x30FF),  # Katakana
    (0x31F0, 0x31FF),  # Katakana Phonetic Extensions
    (0xFF00, 0xFFEF),  # Halfwidth and Fullwidth Forms
    (0xFFFD, 0xFFFD),  # Replacement character
]

# kJis0 = JIS X 0208, kJis1 = JIS X 0212, kJIS0213 = JIS X 0213
JIS_FIELDS = {"kJis0", "kJis1", "kJIS0213"}


def read_unihan(zip_arg):
    if zip_arg:
        return Path(zip_arg).read_bytes()
    print(f"Downloading {UNIHAN_ZIP_URL}", file=sys.stderr)
    with urllib.request.urlopen(UNIHAN_ZIP_URL) as response:
        return response.read()


def jis_codepoints(zip_bytes):
    codepoints = set()
    with zipfile.ZipFile(io.BytesIO(zip_bytes)) as zf:
        with zf.open("Unihan_OtherMappings.txt") as f:
            for line in io.TextIOWrapper(f, encoding="utf-8"):
                parts = line.rstrip("\n").split("\t")
                if len(parts) >= 2 and parts[1] in JIS_FIELDS and parts[0].startswith("U+"):
                    codepoints.add(int(parts[0][2:], 16))
    return codepoints


def main():
    codepoints = jis_codepoints(read_unihan(sys.argv[1] if len(sys.argv) > 1 else None))
    for start, end in FIXED_RANGES:
        codepoints.update(range(start, end + 1))

    with open(OUTPUT_PATH, "w", encoding="utf-8") as f:
        f.write("# Japanese kanji and kana allowed by --codepoints-file\n")
        f.write(f"# Generated {date.today().isoformat()} by generate_japanese_codepoints.py from Unihan 17.0.\n")
        f.write(f"# {len(codepoints):,} codepoints: ASCII, Latin-1, General Punctuation, CJK Symbols and\n")
        f.write("# Punctuation, Hiragana, Katakana, Katakana Phonetic Extensions, Halfwidth and\n")
        f.write("# Fullwidth Forms, U+FFFD, and the ideographs Unihan maps to JIS X 0208, 0212\n")
        f.write("# or 0213 (kJis0, kJis1, kJIS0213).\n")
        for cp in sorted(codepoints):
            f.write(f"{cp:04X}\n")
    print(f"Wrote {len(codepoints)} codepoints to {OUTPUT_PATH}", file=sys.stderr)


if __name__ == "__main__":
    main()
