# Carve DAW

Synapse Orion 風の「Generator 中心・パターンベース」DAW。
エンジンは **tracktion_engine**、ドキュメントは独自の ValueTree モデル (`.carve`, XML)。

**Pattern が第一級**なのが要点: tracktion の Clip は配置ごとのコピーだが、
本アプリではパターンを直せば曲中の全配置に反映される (EditSync が再同期する)。

## できること

- **Generator**: 4OSC (内蔵シンセ、専用エディタ付き) / 808 Drums (内蔵ドラムシンセ:
  キック・リム・スネア・クラップ・ハイハット・タム・カウベル、GM 配列) / VST3・AU /
  サンプラー / ドラムキット (16 パッド、GM 配列) / オーディオトラック
- **パターン**: A1..D9 スロットグリッド、ピアノロール (選択・矩形選択・ズーム・
  ベロシティレーン・クオンタイズ/スウィング・コピペ)、複製、MIDI 入出力
- **プレイリスト**: ペイント/選択ツール、小節ルーラー (ズーム・ループ範囲・
  テンポ/拍子レーン)、配置ごとの長さ (ループ/切り詰め) と transpose、コピペ、
  オーディオファイルのドラッグ&ドロップ (波形表示)
- **ミキサー**: チャンネルストリップ (フェーダー/パン/mute/solo/メーター)、
  insert エフェクトチェーン、センド/リターンバス、マスターバス
- **エフェクト**: tracktion 内蔵 (Compressor/Limiter, EQ, Reverb, Delay,
  Chorus, Phaser, LPF/HPF, PitchShift) + 自前の Distortion + VST3/AU。
  コンプは別 Generator を**サイドチェイン**元にできる
- **オートメーション**: パラメータカーブ (プレイリストの行を展開して編集) と
  LFO モディファイア。どちらも拍基準でテンポ変更に追従
- **その他**: 途中でのテンポ・拍子変更、プリセット保存/読込、WAV 書き出し (⌘E)、
  undo 全対応、`.carve` 保存 (旧 `.orion` も読める)

すべての編集はモデル (ValueTree) を経由するので、undo・保存・再生中の
リアルタイム反映が一様に効く。

## ビルドと起動

```sh
./run.sh                # ビルドして GUI 起動 (初回は tracktion+JUCE 取得で ~500MB)
./run.sh --asan         # AddressSanitizer ビルドで起動
./run.sh --render ...   # ヘッドレスレンダラ (引数はそのまま渡る)
./run.sh --test         # プロパティテスト
```

素の CMake なら `cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel`。

### テスト

Catch2 + RapidCheck による property-based test。既定で有効なので、追加のフラグなしに走る。

```sh
./run.sh --test                       # ビルドして全プロパティを実行
./run.sh --test "[song]"              # タグで絞る (Catch2 の引数はそのまま渡る)
```

プロパティは既定で各 100 ケース。強く回したいときや反例を再現したいときは RapidCheck の環境変数を使う:

```sh
RC_PARAMS="max_success=20000 max_size=200 seed=1" ./run.sh --test
```

対象は「モデル層の算術」「ドキュメントの編集操作（undo/redo・XML 往復を含む）」「ピアノロールのノート移動ジェスチャ」。エンジン（tracktion）と GUI コンポーネントは対象外で、テストバイナリは engine をリンクしない。フェッチを避けたいときは `-DCARVE_BUILD_TESTS=OFF`。

### CLI

```sh
BIN=./build/carve-render_artefacts/Release/carve-render
$BIN --demo demo.wav              # デモ曲をレンダリング
$BIN --write-demo demo.carve      # デモ曲を .carve として書き出し
$BIN song.carve out.wav           # .carve をレンダリング
$BIN --scan                       # VST3/AU をスキャン (GUI と設定を共有)
$BIN --plugin-demo <名前> out.wav  # 名前でマッチした VSTi でデモ曲をレンダ
$BIN --rate 48000 song.carve out.wav  # レートを指定 (既定 44100)
```

`--rate` はどのコマンドにも付けられる。既定は 44100 Hz 固定で、オーディオ
デバイスの設定には左右されない — GUI がデバイスを掴んでいるかどうかで
同じ曲が 44.1k/48k に振れると、md5 比較の回帰チェックが成立しないため。
ブロックサイズも同じ理由で固定 (512)。

## 主なショートカット

各ウィンドウ下部のヘルプバーが「その時できる操作」を表示する。

| キー | 動作 |
|---|---|
| Space | 再生 / 停止 |
| ⌘Z / ⇧⌘Z | undo / redo |
| ⌘S / ⇧⌘S | 上書き保存 / 別名保存 |
| ⌘E | WAV 書き出し |
| ⌘C / ⌘X / ⌘V | コピー / カット / ペースト (ノート・クリップ) |
| ⌘D | 複製 (クリップ。パターンスロットは右クリックメニューから) |
| B / E | ペイント / 選択ツール (プレイリスト) |
| D / E | 描画 / 選択ツール (ピアノロール) |
| ⌘スクロール | ズーム (ポインタ位置を軸に) |

## 実装メモ

- **プラグインスキャンは子プロセス**で走る。読み込みで落ちるプラグインが
  あってもアプリは巻き添えにならず、ブラックリスト入りして続行する
- **CLI レンダラーは決定的**: 同一曲のレンダリングはビット一致する
  (発振位相の乱数シードを固定する自前パッチ `patches/` + 単一スレッド実行
  + サンプルレートとブロックサイズをデバイスから切り離して固定)。
  GUI の再生と書き出しはマルチスレッドのまま
- オーディオクリップは `start` が拍 (音楽的)、`length`/`offset` が秒 (物理的)。
  テンポ変更で位置は動くが、音は伸び縮みしない

### 録音に着手するときの注意

本アプリは録音しないので、`src/EngineSetup.h` でオーディオ入力を開かない
(`shouldOpenAudioInputByDefault() == false`)。これは JUCE 8.0.6 の CoreAudio
バックエンドにある境界外書き込みの回避も兼ねている:

`CoreAudioInternal::reopen()` はデインターリーブ用 temp バッファをデバイスの
実ブロックサイズで確保した後、`bufferSize` を要求値に上書きするが再確保しない。
要求サイズを受け付けないデバイス (例: Bluetooth ヘッドセットの 16kHz/320 フレーム
入力に 512 を要求) があると、コールバックがバッファを溢れてヒープを壊す。
既定の入出力が別デバイスだと JUCE が AudioIODeviceCombiner を作るため踏みやすい。

入力を開くように戻す場合は、`getAvailableBufferSizes()` から要求サイズを選ぶか、
JUCE 側にパッチを当てること。

## ライセンス注意

tracktion_engine は GPLv3 / 商用デュアルライセンス。クローズドソース配布には
商用ライセンスが必要。同梱の AirWindows (189 種、`TRACKTION_AIR_WINDOWS` で
有効化可) は同梱コピーにライセンス文が無いため未使用のまま。
