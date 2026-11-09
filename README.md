# Carve DAW

曲を彫刻のように、盛ったり削ったりしながら作る macOS 用の DAW。
Synapse Audio の **Orion** を参考にした、Generator 中心・パターンベースの作り。
**パターンが第一級**で、パターンを直せば曲中の全配置に反映される。

**[ダウンロード (macOS 11 以降、署名・公証済み)](https://github.com/hashrock/carve-daw/releases/latest)**
・ [紹介ページ](https://hashrock.github.io/carve-daw/)

## 機能

- **楽器**: Slate (内蔵シンセ) / 808 Drums / Gabber Kick / サンプラー / 16 パッドのドラムキット /
  SoundFont (.sf2) / **UTAU** (ピアノロールで歌詞を打つと UTAU 音源が歌う) /
  **Break Slicer** (ドラムループを切って刻む) / VST3・AU / オーディオトラック
- **作曲**: パターンとプレイリスト、ピアノロール、テンポ・拍子の変更、
  オートメーションと LFO
- **ミキサー**: インサートエフェクト、センド / リターン、グループバス、
  サイドチェイン。自前の Overtop (マルチバンドコンプ)・Delay・Limiter など
- **演奏と録音**: MIDI キーボードと PC キーボードで弾き、録音する。録音して
  いなくても直前のフレーズを拾える (Capture)
- **書き出し**: WAV / Ogg Vorbis、ミックスダウンかステム。ゲーム用の
  イントロ付きループ (`LOOPSTART` / `LOOPLENGTH`) にも対応
- **付属**: サンプル曲 3 曲とドラムキット

画面ごとの使い方と各機能の詳細は [docs/GUIDE.md](docs/GUIDE.md)。

## 主なショートカット

各ウィンドウ下部のヘルプバーが「その時できる操作」を表示する。標準的なもの
(Space, ⌘Z, ⌘S, ⌘C/X/V, ⌘スクロールでズーム) は省く。

| キー | 動作 |
|---|---|
| ⌘E | 書き出し (WAV / Ogg、ゲームループ) |
| ⌘R | MIDI 録音の開始 / 停止 (停まっていれば再生も始まる) |
| ⇧⌘R | Capture: 直前に弾いたフレーズを新しいパターンにする |
| ⌘K | PC キーボードで弾く / やめる |
| ⌘, | 設定 (オーディオ出力・MIDI 入力) |
| ⌘B | ブラウザの表示 / 非表示 |
| ⌘D | 複製 (プレイリストではクリップ、Generator ウィンドウではパターン) |
| B / E | ペイント / 選択ツール (プレイリスト)。Shift 押下中は一時的に選択ツールで、ペイント中はクリップの上からでも矩形選択できる |
| D / E | 描画 / 選択ツール (ピアノロール)。Shift 押下中は一時的に選択ツール |
| ⌘ + ノートをドラッグ | 複製して移動 (ドラッグ途中の押下 / 解放でも切り替わる) |
| Ctrl 押下中 | ピアノロール: Snap を一時的に無視 (ドラッグ・描画の途中でも) |
| 右クリック | ピアノロール: 選択中のノート上で選択を削除、それ以外は消しゴム / 行ラベル: Generator のメニュー |
| Alt + ドラッグ | 消しゴム (ピアノロール・プレイリスト) |

## ビルド

```sh
./run.sh            # ビルドして起動 (初回は tracktion + JUCE の取得で ~500MB)
./run.sh --test     # テスト (モデル層)
```

素の CMake なら `cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel`。
テスト・ヘッドレスレンダラ (`carve-render`)・配布の手順と実装メモは
[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md)。

## ライセンス

**AGPL-3.0-or-later** ([LICENSE](LICENSE))。土台の tracktion_engine (GPL3) と
JUCE (AGPL3) から決まる。依存関係の一覧と経緯は [NOTICE.md](NOTICE.md) と
[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md)。
