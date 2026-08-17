# orionish-tracktion

Synapse Orion 風の「Generator 中心・パターンベース」DAW。エンジンは **tracktion_engine**、
モデルは独自の Orion 風 ValueTree ドキュメント(`.orion`)。

## アーキテクチャ

```
src/
├─ model/   Orion風モデル (Song → Generators[] → Patterns[] → Notes + Playlist)
│           ValueTree + 型付きラッパー。undo・.orion (XML) 保存。真実の源。
├─ sync/    EditSync: モデル変更を監視して tracktion Edit へ全再同期
│           (Generator → AudioTrack + 4OSC、Pattern配置 → MidiClip)
├─ app/     GUI: Transportバー / Generator・Patternパネル / ピアノロール / Playlist
└─ RenderMain.cpp   ヘッドレスレンダラ CLI
```

ポイント: tracktion の Clip は配置ごとのコピーだが、本アプリでは **Pattern が第一級**。
パターンを編集すると EditSync が全配置の MidiClip を作り直すので、Orion 流の
「パターンを直せば曲中の全配置に反映」が成立する。

## ビルド

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release   # tracktion_engine + JUCE を自動取得 (~500MB)
cmake --build build --parallel
```

macOS で Xcode.app と Command Line Tools のバージョンが食い違う環境では、
CC/CXX/SDKROOT を CLT 側に固定して configure する(orionish 本体の README 参照)。

## 使い方

```sh
# GUI (デモ曲入りで起動)
open "build/orionish-te_artefacts/Release/Orionish TE.app"

# CLI
BIN=./build/orionish-te-render_artefacts/Release/orionish-te-render
$BIN --demo demo.wav              # デモ曲をレンダリング
$BIN --write-demo demo.orion      # デモ曲を .orion として書き出し
$BIN demo.orion out.wav           # .orion をレンダリング
$BIN some.tracktionedit out.wav   # 素の tracktion edit もレンダリング可
$BIN --scan                       # VST3/AU をスキャン (結果は設定に永続化)
$BIN --plugin-demo DLS out.wav    # 名前でマッチした VSTi/AU でデモ曲をレンダ
```

### GUI 操作

- **+ Generator**: 4OSC(内蔵)またはスキャン済み VST3/AU インストゥルメントを選択。
  「Scan / manage plugins...」でスキャン画面を開ける
- **Instrument UI**: 選択中 Generator のプラグインエディタを開く(外部プラグインのみ)
- **ピアノロール**: 空セルをクリックでノート追加 / ドラッグで移動 / 右端ドラッグで長さ変更 /
  右クリック(または⌥クリック)で削除
- **Playlist**: 空きをクリックで選択中パターンを配置(小節スナップ)/ クリップをクリックで削除 /
  行ラベルクリックで Generator 選択
- **Space** 再生/停止、**⌘Z / ⇧⌘Z** undo/redo
- 編集は再生中でもリアルタイムに反映される(EditSync が同期)

## ロードマップ

- [x] モデル層 + EditSync + パターン編集 GUI + 再生
- [x] VST3/AU ホスティング (スキャン、Generator として選択、エディタ表示、状態の保存/復元)
- [ ] 4OSC のパッチ編集 UI
- [ ] ミキサービュー (volume/pan/insert、tracktion の Plugin をそのまま活用)
- [ ] オートメーション (AutomatableParameter + カーブ編集)
- [ ] オーディオトラック・録音
- [ ] パターンのステップシーケンサ表示 (Orion 流のもう一つの編集モード)

## ライセンス注意

tracktion_engine は GPLv3 / 商用デュアルライセンス。クローズドソース配布には
商用ライセンスが必要。
