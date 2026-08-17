# orionish-tracktion

Synapse Orion 風の「Generator 中心・パターンベース」DAW。エンジンは **tracktion_engine**、
モデルは独自の Orion 風 ValueTree ドキュメント(`.orion`)。

## アーキテクチャ

```
src/
├─ model/   Orion風モデル (Song → Generators[] → Patterns[] → Notes + Playlist)
│           ValueTree + 型付きラッパー。undo・.orion (XML) 保存。真実の源。
├─ sync/    EditSync: モデル変更を監視して tracktion Edit へ全再同期
│           (Generator → AudioTrack + 4OSC + ミキサー状態、Pattern配置 → MidiClip)
├─ app/     GUI: Transportバー / Generator・Patternパネル / ピアノロール / ミキサー / Playlist
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
- **Edit Pattern**(または Generator をダブルクリック): パターンエディタをポップアップで開く。
  選択中のパターンに追従する
- **ピアノロール**(ポップアップ): 空セルをクリックでノート追加 / ドラッグで移動 /
  右端ドラッグで長さ変更 / 右クリック(または⌥クリック)で削除
- **Mixer**(トランスポートバー): Generator ごとのチャンネルストリップをポップアップで開く。
  音量フェーダー / パン / Mute / Solo とポストフェーダーのレベルメーター。
  フェーダーとパンはダブルクリックで既定値に戻る
- **Playlist**: 空きをクリックで選択中パターンを配置(小節スナップ)/ クリップをクリックで削除 /
  行ラベルクリックで Generator 選択
- **Space** 再生/停止、**⌘Z / ⇧⌘Z** undo/redo
- 編集は再生中でもリアルタイムに反映される(EditSync が同期)

## ロードマップ

- [x] モデル層 + EditSync + パターン編集 GUI + 再生
- [x] VST3/AU ホスティング (スキャン、Generator として選択、エディタ表示、状態の保存/復元)
- [x] ミキサー (volume/pan/mute/solo + レベルメーター、ポップアップウィンドウ)
- [ ] 4OSC のパッチ編集 UI
- [ ] ミキサーの insert エフェクトスロット
- [ ] オートメーション (AutomatableParameter + カーブ編集)
- [ ] オーディオトラック・録音 (下記の注意点あり)

### 録音に着手するときの注意

本アプリは録音しないので、`src/EngineSetup.h` の `EngineBehaviour` で
`shouldOpenAudioInputByDefault()` を false にし、オーディオ入力を開かないように
している。これは JUCE 8.0.6 の CoreAudio バックエンドにある境界外書き込みの回避も
兼ねている:

`CoreAudioInternal::reopen()` はデインターリーブ用の temp バッファをデバイスの
実ブロックサイズで確保した後、`bufferSize` を**要求値**に上書きするが再確保しない。
そのため要求サイズを受け付けないデバイスがあると、コールバックが小さいバッファに
要求サイズ分を書き込んでヒープを壊す。既定の入力と出力が別デバイスだと JUCE が
`AudioIODeviceCombiner` を作って両方に同じサイズを要求するので、これを踏みやすい
(Bluetooth ヘッドセットの入力が 16kHz / 320 フレームなのに 512 を要求する等)。

入力を開くように戻す場合は、`getAvailableBufferSizes()` が返す値から要求サイズを
選ぶか、JUCE 側にパッチを当てる対応が必要。

## ライセンス注意

tracktion_engine は GPLv3 / 商用デュアルライセンス。クローズドソース配布には
商用ライセンスが必要。
