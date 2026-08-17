# orionish-tracktion

[orionish](../orionish)(自作エンジン版)と比較するための **tracktion_engine スパイク**。

同じデモ曲(Bass/Chords/Lead、16拍、120BPM)を tracktion_engine の Edit として組み立てて
ヘッドレスレンダリングする。Orion 的な概念のマッピング:

| Orion 概念 | tracktion_engine |
|---|---|
| Generator | `AudioTrack` + インストゥルメントプラグイン (`FourOscPlugin` / 外部VSTi) |
| Pattern | `MidiClip` の中身 (`MidiList`)。配置ごとにクリップとして展開 |
| Playlist | Edit のタイムラインへの Clip 配置 |
| Mixer | トラック標準の `VolumeAndPanPlugin` + `pluginList` (Insert) + AuxSend/Return |
| Automation | `AutomatableParameter` + カーブ (全プラグインパラメータが最初から対応) |
| Transport | `TransportControl` + `TempoSequence` (テンポマップ・拍子込み) |
| 保存形式 | `.tracktionedit` (ValueTree XML、ネイティブでシリアライズ可能) |

## ビルド

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release   # tracktion_engine + JUCE を自動取得 (~500MB)
cmake --build build --parallel
```

macOS で Xcode.app と Command Line Tools のバージョンが食い違う環境では
orionish 本体の README と同じく CC/CXX/SDKROOT を CLT に固定して configure する。

## 使い方

```sh
BIN=./build/orionish-te-render_artefacts/Release/orionish-te-render
$BIN --demo demo.wav                  # デモ曲をレンダリング
$BIN --write-demo demo.tracktionedit  # デモ曲を .tracktionedit として保存
$BIN demo.tracktionedit out.wav       # Edit ファイルをレンダリング
```

## メモ

- tracktion_engine は **GPLv3 / 商用デュアルライセンス**。クローズドソースで出すなら要ライセンス契約。
- Waveform の実エンジンなので、プラグインホスティング・オートメーション・録音・
  タイムストレッチ等がすべて既製。一方でエンジン内部の学習・改造の自由度は下がる。
