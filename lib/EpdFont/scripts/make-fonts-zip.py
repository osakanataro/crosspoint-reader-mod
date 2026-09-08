#!/usr/bin/env python3
"""Assemble the OST版 SD-card font archive, fonts-YYYYMMDD<n>.zip, from a build-sd-fonts.py
output tree, and put the attribution the SIL Open Font License asks for next to the fonts:

  fonts/<Family>/<Family>_<size>.cpfont   the converted fonts (as before)
  fonts/NOTICE.txt                        per family: source fonts, their URLs, versions,
                                          copyright holders, the fallback fonts, and what
                                          the conversion did
  fonts/OFL-1.1.txt                       the licence text every source is published under

The version / copyright / licence lines are read from the name tables of the downloaded
source fonts (scripts/downloaded_fonts/<Family>/...) when they are present, so NOTICE.txt
records what was actually converted rather than what the URL points at today. Reserved Font
Names, if a source ever declares one, are reported too: a converted font may not carry such
a name, and this archive names its files after the source families.

    python3 make-fonts-zip.py --fonts-dir ../../../fonts-out --date 20260908 --serial 0
"""
import argparse, datetime, io, sys, zipfile
from pathlib import Path

import yaml

SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_CONFIG = SCRIPT_DIR / "sd-fonts.yaml"
DEFAULT_SOURCES = SCRIPT_DIR / "downloaded_fonts"
DEFAULT_FALLBACK = SCRIPT_DIR.parent / "builtinFonts/source/NotoSans"
OFL_START = "SIL OPEN FONT LICENSE Version 1.1"


def name_table(path: Path) -> dict:
    """Version / copyright / licence / reserved-name facts from a font file, or {}."""
    try:
        from fontTools.ttLib import TTFont
    except ImportError:
        return {}
    if not path.exists():
        return {}
    names = TTFont(str(path), lazy=True)["name"]

    def get(nid):
        rec = names.getName(nid, 3, 1) or names.getName(nid, 1, 0)
        return str(rec).strip() if rec else ""

    licence = get(13)
    return {
        "version": get(5),
        "copyright": get(0),
        "license_url": get(14),
        "ofl": "Open Font License" in licence,
        "reserved": "Reserved Font Name" in licence,
    }


def source_file(sources: Path, family: str, spec) -> Path:
    url = spec["url"] if isinstance(spec, dict) else spec
    return sources / family / url.rsplit("/", 1)[-1]


def describe_family(fam: dict, sources: Path, fallback_dir: Path, present: list) -> str:
    name = fam["name"]
    out = [f"== {name} ==", f"  {fam.get('description', '')}".rstrip(), "  files:"]
    out += [f"    {p}" for p in present]
    out.append("  source fonts (converted from):")
    for style, spec in fam.get("styles", {}).items():
        url = spec["url"] if isinstance(spec, dict) else spec
        info = name_table(source_file(sources, name, spec))
        out.append(f"    {style:<7} {url}")
        if info:
            out.append(f"            version: {info['version']}")
            out.append(f"            copyright: {info['copyright']}")
            out.append(f"            licence: {'SIL OFL 1.1' if info['ofl'] else 'see font'}"
                       f"{' (declares a Reserved Font Name -- check before naming derived files)' if info['reserved'] else ' (no Reserved Font Name declared)'}")
        else:
            out.append("            (source file not on hand; version not recorded)")
    fb = []
    ofl = fallback_dir / "OFL.txt"
    if ofl.exists():
        first = ofl.read_text(encoding="utf-8").splitlines()[0].strip()
        fb.append(f"    Noto Sans (Latin/Greek/Cyrillic fallback, bundled with CrossPoint): {first}")
    for spec in fam.get("extra_fallbacks", []):
        url = spec["url"] if isinstance(spec, dict) else spec
        info = name_table(source_file(sources, name, spec))
        line = f"    {url}"
        if info:
            line += f"\n            version: {info['version']}; copyright: {info['copyright']}"
        fb.append(line)
    if fb:
        out.append("  fallback fonts (only for code points the source lacks):")
        out += fb
    lists = fam.get("codepoints_file", [])
    if lists:
        out.append("  code points kept: " + ", ".join(Path(p).name for p in lists)
                   + f" (plus intervals: {fam.get('intervals', '')})")
    return "\n".join(out)


def build_notice(families: list, sources: Path, fallback_dir: Path, tree: Path, stamp: str) -> str:
    head = f"""OST版 CrossPoint Reader SD カード用フォント / OST-edition SD-card fonts
archive: fonts-{stamp}.zip   generated: {datetime.date.today().isoformat()}

このアーカイブの .cpfont は、下記の元フォントから CrossPoint Reader 用の点画像形式に
変換したものです（収録範囲は JIS X 0213 などの符号位置リストで絞り、太字は元フォントの
太字から生成。字形の変更はしていません）。元フォントはすべて SIL Open Font License 1.1
（同梱の OFL-1.1.txt）で公開されており、この変換物も同じ条件で配布します。
著作権は各元フォントの著作権者にあります。

The .cpfont files here are bitmap conversions of the source fonts listed below for
CrossPoint Reader (glyphs subset by code-point lists such as JIS X 0213; bold from the
sources' own bold faces; glyph shapes unchanged). Every source is published under the
SIL Open Font License 1.1 (OFL-1.1.txt in this folder) and this archive is distributed
under the same terms. Copyright remains with the respective copyright holders.

生成元 / built by: lib/EpdFont/scripts/build-sd-fonts.py + make-fonts-zip.py
(https://github.com/osakanataro/crosspoint-reader-mod, branch feat/japanese-sd-fonts)
"""
    blocks = []
    for fam in families:
        present = sorted(p.name for p in (tree / fam["name"]).glob("*.cpfont"))
        blocks.append(describe_family(fam, sources, fallback_dir, present))
    return head + "\n" + "\n\n".join(blocks) + "\n"


def ofl_text(fallback_dir: Path) -> str:
    src = (fallback_dir / "OFL.txt").read_text(encoding="utf-8")
    body = src[src.index(OFL_START):]
    return ("This licence applies to every font in this archive. The copyright holders and\n"
            "any Reserved Font Names of each source are recorded in NOTICE.txt.\n\n"
            "この文書はアーカイブ内のすべてのフォントに適用されます。各元フォントの著作権者は\n"
            "NOTICE.txt に記載しています。\n\n-----\n\n" + body)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fonts-dir", required=True, type=Path, help="build-sd-fonts.py output tree (<Family>/*.cpfont)")
    ap.add_argument("--date", default=datetime.date.today().strftime("%Y%m%d"))
    ap.add_argument("--serial", type=int, default=0, help="same-day rebuild counter, starts at 0")
    ap.add_argument("--out-dir", type=Path, default=Path("."))
    ap.add_argument("--config", type=Path, default=DEFAULT_CONFIG)
    ap.add_argument("--sources", type=Path, default=DEFAULT_SOURCES, help="downloaded source fonts")
    ap.add_argument("--fallback-dir", type=Path, default=DEFAULT_FALLBACK)
    ap.add_argument("--families", nargs="*", help="subset of families to pack (default: all present)")
    args = ap.parse_args()

    config = yaml.safe_load(args.config.read_text(encoding="utf-8"))
    families = [f for f in config["families"] if (args.fonts_dir / f["name"]).is_dir()]
    if args.families:
        families = [f for f in families if f["name"] in args.families]
    if not families:
        sys.exit(f"no family directories found under {args.fonts_dir}")

    stamp = f"{args.date}{args.serial}"
    out = args.out_dir / f"fonts-{stamp}.zip"
    notice = build_notice(families, args.sources, args.fallback_dir, args.fonts_dir, stamp)
    licence = ofl_text(args.fallback_dir)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for fam in families:
            for p in sorted((args.fonts_dir / fam["name"]).glob("*.cpfont")):
                z.write(p, f"fonts/{fam['name']}/{p.name}")
        z.writestr("fonts/NOTICE.txt", notice)
        z.writestr("fonts/OFL-1.1.txt", licence)
    print(f"wrote {out} ({out.stat().st_size // 1024} KB, {len(families)} families)")
    print(notice)


if __name__ == "__main__":
    main()
