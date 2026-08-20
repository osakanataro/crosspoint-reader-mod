# CrossPoint Reader OST 改造版

XTEINK X3上で日本語EPUBファイルを読むために、CrossPoint Reader を改造しています。
命名は面倒くさくなったのでOSakanaTaro版ってことでOST版です。

### 2026/08/20時点の開発方針
- XTEINK X3で動けばいい
  - aliexpressで買ったUSB接続ができないバージョンを使用
- Claude Codeで開発する
  - コンパイルなどはUbuntu 24.04仮想マシン上で実施
- 本家をベースにする
  - X3の最近のバージョンは液晶パネルが違う件への対応などが本家には入っているため
  - 初期は1.5.0ベースで開始。本家にXteink x4 pro対応が入ったあたりで乖離が始まった
  - 2026/08/20から1.6.0rcをベースにして再構成開始
- 日本語訳は付けない
- フォントはSDカード上に配置する
  - 容量食うのでSDカード上に置く
- ページめくりボタンは左が進むで、右が戻る
  - 縦書きフォーマット(RTL)のEPUBを開いた時だけその動作をする
- テスト用EPUBファイルにて動作を確認
  - 一部、特殊なフォントを必要とする文字以外について表示できることが期待される
- ルビ表示について対応中
  - 横書きであればルビに対応してるなら縦書きでも対応しておくかな、って
- 電書連 EPUB 3 制作ガイド ver.1.1.4に準拠したEPUB表示についての対策は悩み中
  - CSSが巨大すぎてメモリクラッシュを起こす件についての対応をやる必要があるのかという問題
  - [Setings]-[Reader]-[Text Settings]にある[Style]にて「Embedded Style」を「OFF」にするとCSSを使わないため回避できる
  - こんな小さな端末で著者がやりたいという複雑な表現をやって期待通りの表示になるのか？
- 開発に際して下記URLをClaude codeに読み込ませている
  - 本家 https://github.com/crosspoint-reader/crosspoint-reader
  - JP版 https://github.com/zrn-ns/crosspoint-jp
  - CJK版 https://github.com/aBER0724/crosspoint-reader-cjk
  - Yomuka版 https://github.com/ponto1216-ai/crosspoint-jp
  - 抹茶版 https://github.com/eszter007/matcha-reader


## 2026/08/20までの実装内容まとめ by claude code

ブランチ `feat/vertical-1.6`。本家タグ `1.6.0rc`（`6a501bba`）を起点に、旧ツリー
（`feat/vertical-writing-foundation`、1.5.0世代ベース）の成果を選別して再構成したもの。
旧ツリーが個別にcherry-pickしていた本家修正（#3071 CJKリスト高速化、#3034 ファイル一覧の
レース修正、#3026 SDフォールバック一括warm、#3001 EPUBメタデータの名前空間対応）は
1.6.0rcに全て含まれているため、移植対象から除外した。

### 診断基盤（INPUT_DIAG）の移植
- シリアルコンソールが使えないX3向けの、SDカードへ書き出す入力・描画計測
  （`/input-diag.txt`、`src/util/InputDiag.*`、`-diag`ビルド命名の`scripts/ost_version.py`）
- 1.6系で追加: プリウォーム経路の計測（`page_scan_last`/`prewarm_entry_fails`）と、
  本を開く6段階のヒープ記録（`/open-heap.txt`。クラッシュしても直前までの段階が残る）

### メモリ断片化・クラッシュ対策
- グリフ用アドバンステーブルの収集バッファを16KB一括確保から256件開始の倍々成長に変更
  （断片化ヒープでの章ジャンプ7〜12秒/クラッシュの原因だった）
- グリフビットマップのミニアリーナを4KBチャンク分割確保に変更
  （本家未マージの`feat-chunk-bitmaps`由来。最大連続5KBの環境でもプリウォームが成立する）
- リスト画面（章選択・ブックマーク等）を開いた時点で全SDフォントキャッシュを解放
  （読書フォントの常駐数十KBを返してから章ジャンプの構築が走る）
- ページ読み込み前の低ヒープガード: ビルド進行中に空き20KB未満なら`suspendBuild()`で
  構築を部分ファイルとして退避（既存の遅延再開機構が引き継ぐ）、12KB未満でフォントキャッシュ解放

### 出版社CSSによるメモリ枯渇の解消（JP版 #103/#105/#106 の移植）
- 角川系テンプレートCSS（実測144KB・約1,865ルール）が構築時に丸ごとRAM展開されて
  ヒープを食い尽くし、`-fno-exceptions`下の`bad_alloc`→`abort()`で再起動する問題への対策
- `CssSelectorUsage`: 章のHTMLを走査して使用タグ/クラス集合を作り、マッチするルールだけを
  RAMにロード。`Epub::load`のキャッシュ検証もヘッダのみの`validateCache()`に変更
- CSSパース中の32KBヒープガード3箇所と、打ち切られた不完全ルール集合をキャッシュに
  保存しない対策（オリジナルはJP版 zrn-ns氏）
- 効果（実測）: 構築の消費 約95KB→約24KB、読書中の空きヒープ 15KB→74〜90KB、クラッシュ解消

### 縦書き（tategaki）コアの移植
- UAX#50準拠の文字向き分類・縦書き句読点配置・禁則処理（`VerticalTextUtils.h`）
- 縦組版エンジン（`layoutVerticalColumns`: 右→左の列組み、列境界の禁則押し戻し、
  1〜2桁数字の縦中横、`3.14`等の数値非分断、Latin語の横倒し、語間スペースの保持）
- 縦書き描画（`renderVertical`: セル配置、`Sideways90CW`回転描画、ルビの計測・配置・描画）
- 縦書き判定: spineの`page-progression-direction="rtl"`かつCJK言語で自動有効
- ページ送り方向の反転（ボタン・タップゾーン・スワイプすべてRTL準拠）
- キャッシュ形式: `SECTION_FILE_VERSION` 39→40、`BOOK_CACHE_VERSION` 10→11
- rc側で進んでいた #2892（フォーカス分割の新データモデル）、#3001、#3025（リーダー統合）、
  スワイプ操作に適応する手移植（機械的なcherry-pickは不可能だった）

### 実機での状態（X3、日本語SDフォント・商業EPUB）
- 縦書き表示・ルビ・RTLページ送り動作、横書きの回帰なし
- ページ送り 約1.3〜1.6秒、定常時のオンデマンドグリフ読み 0
- 読書中の空きヒープ 76〜90KB / 最大連続 45〜57KB

### 未対応・後回し（旧ツリーからの引き継ぎ課題含む）
- 時計表示機能の移植（次の作業）
- ルビの品質改善（漢字ルビの可読性、行分割時のルビ配分など）と縦書きの字下げ
- 縦書きページの読書位置同期が段落単位でずれる件（visible-offsetの粒度）
- textAntiAliasing既定ONの性能問題への対応方針

# 以下 crosspoint reader公式 のコピー
# CrossPoint Reader

[![Fund contributors](https://img.shields.io/badge/%F0%9F%91%91_Fund_contributors-royalty.dev-BB953A?style=for-the-badge&labelColor=1a1a1a)](https://app.royalty.dev/crosspoint-reader/crosspoint-reader)

CrossPoint is open-source e-reader firmware - community-built, fully hackable, free forever. It's maintained by a growing community of developers and readers who believe your device should do what you want - not what a manufacturer decided for you.

**Now running on:** ESP32C3-based Xteink [X4](https://www.xteink.com/products/xteink-x4) and [X3](https://www.xteink.com/products/xteink-x3).

![CrossPoint Reader running on Xteink device](./docs/images/cover.jpg)

> If you're planning to buy an Xteink device, consider purchasing an **X3/X4 Developer Edition** through https://crosspointreader.com. CrossPoint receives a small share of each sale, helping fund development costs.

## What can CrossPoint do?

- **Reader engine**: EPUB 2/3 rendering with embedded-style option, image handling, hyphenation, kerning, chapter navigation, footnotes, bookmarks, dictionary lookups ([StarDict](docs/dictionary.md)), go-to-percent, auto page turn, orientation control, focus reading, KOReader progress sync and more. 

- **Various formats**: native handling for `.epub`, `.xtc/.xtch`, `.txt`, and `.bmp`.

- **Screenshots.**

- **Custom fonts**: install your favorite fonts on the SD card.

- **Tilt page turn (X3 only)**.

- **Library workflow**: folder browser, hidden-file toggle, long-press delete, recent books, SD-cache management.

- **Wireless workflows**:
  
  - File transfer web UI
  - EPUB Optimizer
  - Web settings UI/API (edit many device settings from browser)
  - WebSocket fast uploads
  - WebDAV handler
  - AP mode (hotspot) and STA mode (join existing Wi-Fi), both with QR helpers
  - Calibre wireless connect flow
  - OPDS browser with saved servers (up to 8), search, pagination, and direct download
  - OTA update checks and installs from GitHub releases

- **Customization**: multiple themes (Classic, Lyra, Lyra Extended, RoundedRaff), sleep screen modes including transparent overlays, front/side button remapping, status bar controls, power-button behavior, refresh cadence, and more.

- **Localization**: 24 UI languages and counting. RTL support.

### Coming soon:

- More themes.

- Much more! stay tuned.

---

## USB-locked devices (Xteink Unlocker)

Some Xteink units purchased from third-party stores (e.g. AliExpress) ship with USB flashing locked from the factory.
If your device is locked, you will need to use the **Xteink Unlocker** tool available at
https://crosspointreader.com/#unlock-tool before you can flash CrossPoint.

**You do not need this tool if you bought your device directly from xteink.com.** Those units are not locked.

**Not sure if your device is locked?** Power it on, connect the USB-C cable, and try flashing via the web flasher first (see
[Install firmware](#install-firmware) below). If the browser's serial device picker does not show your device, try a different
USB port or browser before assuming the device is locked. Only reach for the unlocker if the device still doesn't appear.

> ### ⚠️ WARNING: READ THIS BEFORE USING THE UNLOCKER ⚠️
> 
> **The only officially supported firmwares in the unlock tool are CrossPoint and CrossInk.**
> 
> Flashing any other firmware on a USB-locked device may **permanently brick the device** or leave it **permanently
> stuck on that firmware with no recovery path**. Once USB flashing is re-locked, your only way back is via OTA, and if
> the firmware you flashed doesn't support OTA, **there is no way out**.

## Install firmware

### Web installer (recommended)

1. Connect your device to your computer via USB-C and wake/unlock the device
2. Go to https://crosspointreader.com/#flash-tools, select device (X3 or X4), and choose an official CrossPoint release.

### Web installer (specific version)

1. Connect your device to your computer via USB-C and wake/unlock the device
2. Download a `firmware.bin` from [Releases](https://github.com/crosspoint-reader/crosspoint-reader/releases), local build, or continuous integration artifact.
3. Go to https://crosspointreader.com/#flash-tools, select device (X3 or X4), click "Custom .bin" and upload a `firmware.bin`.

### Revert to Official Firmware

To revert to the official firmware, you can also flash the latest official firmware using https://crosspointreader.com/#flash-tools.

### Command line

1. Install [`esptool`](https://github.com/espressif/esptool):

```bash
pip install esptool
```

2. Download `firmware.bin` from the [releases page](https://github.com/crosspoint-reader/crosspoint-reader/releases).
3. Connect your device via USB-C.
4. Find the device port. On Linux, run `dmesg` after connecting. On macOS:

```bash
log stream --predicate 'subsystem == "com.apple.iokit"' --info
```

5. Flash:

```bash
esptool.py --chip esp32c3 --port /dev/ttyACM0 --baud 921600 write_flash 0x10000 /path/to/firmware.bin
```

Adjust `/dev/ttyACM0` to match your system.

### Manual

See [Development quick start](#development-quick-start) below.

---

## Custom SD-card fonts

Convert your own TTF/OTF files into `.cpfont` files that load from the SD card. No firmware reflash is needed.

1. Go to https://crosspointreader.com/fonts and open the "SD-card font builder" form.
2. Upload up to four styles (regular, bold, italic, bold-italic), set the family name, point sizes, and Unicode range.
3. Download the generated `.cpfont` files.
4. Copy them to your SD card under `/fonts/YourFont/` (or `/.fonts/YourFont/` to hide the folder).
5. Select the font on the device from the font settings.

Conversion runs the firmware repo's `lib/EpdFont/scripts/fontconvert_sdcard.py` script unmodified, so output matches a local host build.

---

## Documentation

- [User Guide](./USER_GUIDE.md)
- [Web server usage](./docs/webserver.md)
- [Web server endpoints](./docs/webserver-endpoints.md)
- [Project scope](./SCOPE.md)
- [Contributing docs](./docs/contributing/README.md)
- [Touch and UI development](./docs/contributing/touch-and-ui.md) - how to build new screens on the FreeInkUI activity bases (UiListActivity and friends), plus build envs for the non-Xteink touch devices

---

## Development quick start

### Prerequisites

- [pioarduino](https://github.com/pioarduino/pioarduino) or VS Code + pioarduino plugin
- Python 3.8+
- `clang-format` 21
- USB-C cable supporting data transfer

### Setup

```bash
git clone --recursive https://github.com/crosspoint-reader/crosspoint-reader
cd crosspoint-reader

# if cloned without --recursive:
git submodule update --init --recursive
```

### Nix/NixOS

Nix/NixOS users can enter the development shell with either `nix develop` (flakes) or `nix-shell`:

```bash
nix develop -f nix
# or
nix-shell nix
```

To flash a connected ESP32-C3 device, enable PlatformIO's udev rules in your NixOS configuration:

```nix
services.udev.packages = with pkgs; [ platformio-core.udev ];
```

After rebuilding the system configuration, reconnect the device or reload udev rules.

### Build / flash / monitor

```bash
pio run --target upload
```

### Contributor pre-PR checks

```bash
./bin/clang-format-fix
pio check -e default
pio run -e default
```

### Debugging

After flashing the new features, it’s recommended to capture detailed logs from the serial port.

First, make sure all required Python packages are installed:

```python
python3 -m pip install pyserial colorama matplotlib
```

After that run the script:

```sh
# For Linux
# This was tested on Debian and should work on most Linux systems.
python3 scripts/debugging_monitor.py

# For macOS
python3 scripts/debugging_monitor.py /dev/cu.usbmodem2101
```

Minor adjustments may be required for Windows.

---

## Internals

CrossPoint Reader is pretty aggressive about caching data down to the SD card to minimise RAM usage. The ESP32-C3 only has ~380KB of usable RAM, so we have to be careful. A lot of the decisions made in the design of the firmware were based on this constraint.

### Data caching

The first time chapters of a book are loaded, they are cached to the SD card. Subsequent loads are served from the
cache. This cache directory exists at `.crosspoint` on the SD card. The structure is as follows:

```text
.crosspoint/
├── epub_<hash>/         # one directory per book, named by content hash
│   ├── progress.bin     # reading position (chapter, page, etc.)
│   ├── cover.bmp        # generated cover image
│   ├── book.bin         # metadata: title, author, spine, TOC
│   ├── css_rules.cache  # parsed CSS rule cache
│   ├── img_*            # rendered image cache files
│   └── sections/        # per-chapter layout cache
│       ├── 0.bin
│       ├── 1.bin
│       └── ...
├── settings.json        # device settings
├── state.json           # resume/runtime state
└── recent.json          # recent books list
```

Removing `/.crosspoint` clears all cached metadata and forces a full regeneration on next open. Book deletes, overwrites, and moves done through the firmware or web UI clear or re-key matching caches; manual SD-card edits may leave stale cache directories behind.

For more details on the internal file structures, see the [file formats document](./docs/file-formats.md).

---

## Contributing

Contributions are welcome. If you're new to the codebase, start with the [contributing docs](./docs/contributing/README.md). For things to work on, check the [ideas discussion board](https://github.com/crosspoint-reader/crosspoint-reader/discussions/categories/ideas) — leave a comment before starting so we don't duplicate effort.

Everyone here is a volunteer, so please be respectful and patient. For governance and community expectations, see [GOVERNANCE.md](./GOVERNANCE.md).

---

## Community forks

One of the best things about open source is that anyone can take the code in a different direction. If you need something outside CrossPoint's [scope](./SCOPE.md), check out the community forks:

- [CrossInk](https://github.com/uxjulia/CrossInk) — Typography and reading tracking: Bionic Reading (bolds word stems to create fixation points), guide dots between words, improved paragraph indents, and replaces the default fonts with ChareInk/Lexend/Bitter.

- [papyrix-reader](https://github.com/bigbag/papyrix-reader) — Adds FB2 and MD format support. Actively maintained with Arabic script support. Custom themes via SD card.

- ~~[crosspet](https://github.com/trilwu/crosspet) — A Vietnamese fork that adds a Tamagotchi-style virtual chicken that grows based on your reading milestones (pages read, streaks, care). Also: Flashcards, Weather, Pomodoro timer, and mini-games.~~ (Unmaintained)

- [crosspoint-reader-cjk](https://github.com/aBER0724/crosspoint-reader-cjk) — Purpose-built for Chinese, Japanese, and Korean reading.

- [inx](https://github.com/obijuankenobiii/inx) — Completely reimagines the user interface with tabbed navigation.

- ~~[PlusPoint](https://github.com/ngxson/pluspoint-reader) — custom JS apps support.~~ (Unmaintained)

- [crosspoint-reader-papers3](https://github.com/juicecultus/crosspoint-reader-papers3) — Crosspoint port for M5Stack Paper S3. 

- [t5s3-reader](https://github.com/ShallowGreen123/t5s3-reader) — Crosspoint port for LilyGo T5 ePaper S3 / T5S3 4.7-inch e-paper device.

**Note:** Many of these features will make their way into CrossPoint over time. We maintain a slower pace to ensure rock-solid stability and squash bugs before they reach your device.

Want to build your own device? Be sure to check out the [de-link](https://github.com/iandchasse/de-link) project.

---

CrossPoint Reader is **not affiliated with Xteink or any device manufacturer**.

Huge shoutout to [diy-esp32-epub-reader](https://github.com/atomic14/diy-esp32-epub-reader), which inspired this project.
