# 模擬環境での画面の回帰検査

パソコン上の模擬環境（`platformio.ini` の `simulator_x3` など、本家 crosspoint-simulator の
OST fork）で決まった本を開いて決まった操作をし、画面を前回の基準と見比べる。組版や描画を
変えたとき、変えたつもりのない所が変わっていないかを実機の前に確かめるためのもの。

リポジトリの直下で実行する。`<fonts>` は SD 書体（`<Family>/<Family>_<size>.cpfont`）の
ある場所で、配布している fonts zip を展開した物でよい（環境変数 `OST_SIM_FONTS` に入れて
おけば `--fonts-dir` は省ける）。

```bash
cd crosspoint-reader-mod

# 比較（基準と違う画面があれば終了コード 1）
python3 scripts/sim_regress.py --fonts-dir <fonts>

# 変更が意図どおりなら基準を更新する
python3 scripts/sim_regress.py --fonts-dir <fonts> --update-baseline

# 一部だけ、別の機種で
python3 scripts/sim_regress.py --fonts-dir <fonts> --only vertical-ja --env simulator_x4_pro

# 模擬環境の実行ファイルが無いときは --build で組み立てる（pio は $PIO か PATH から）
PIO=/path/to/pio python3 scripts/sim_regress.py --fonts-dir <fonts> --build
```

- `scenarios.yaml` が本・操作・画面取得の指示書。`books/` の本を開き、`steps` を順に実行する
  （`shot:` 画面取得、`press:` ボタン、`wait:` 待ち）。縦書きの本は UP でページが進む
- `baseline/<env>/<scenario>/<shot>.png` が基準。X3（528×792）と X4 Pro（480×800）は別
- 結果は `.pio/sim-regress/<時刻>/` に入る。`index.html` で基準・今回・差分（赤）を並べて見られる。
  `report.md` に一覧
- SD 書体（`.cpfont`）はリポジトリに無い。初回に模擬 SD（`.pio/sim-regress/sd/`）へ写され、2回目からは写し直さない
- 必要な物: SDL2 の開発用部品（`sudo apt install libsdl2-dev libssl-dev`）、Python の Pillow と PyYAML

画面は時刻で取るので、組み立てに時間のかかる本は `settle_ms` を長めにする。模擬環境は
パネルの更新時間・残像・実機のヒープ量を再現しないので、速さや記憶の検査には使えない。
