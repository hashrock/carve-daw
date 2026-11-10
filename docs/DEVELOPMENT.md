# Carve DAW 開発メモ

ビルド・テスト・CLI・配布の詳細と、実装上の判断の記録。README には最短の
手順だけを置いている。

## ビルドと起動

```sh
./run.sh                # ビルドして GUI 起動 (初回は tracktion+JUCE 取得で ~500MB)
./run.sh --asan         # AddressSanitizer ビルドで起動
./run.sh --render ...   # ヘッドレスレンダラ (引数はそのまま渡る)
./run.sh --test         # プロパティテスト (モデル層)
./run.sh --sync-test    # プロパティテスト (エンジンに同期した結果)
```

素の CMake なら `cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel`。

ロゴを変更するときは `carve.svg` を編集して `python3 scripts/generate-icons.py` を実行する。
アプリ内ロゴ・アプリアイコン・Web 用ロゴと favicon を一括更新する（librsvg が必要。
macOS では `brew install librsvg`）。その後のビルドで macOS の `.icns` にも反映される。

## テスト

Catch2 + RapidCheck による property-based test。既定で有効なので、追加のフラグなしに走る。

```sh
./run.sh --test                       # ビルドして全プロパティを実行
./run.sh --test "[song]"              # タグで絞る (Catch2 の引数はそのまま渡る)
```

プロパティは既定で各 100 ケース。強く回したいときや反例を再現したいときは RapidCheck の環境変数を使う:

```sh
RC_PARAMS="max_success=20000 max_size=200 seed=1" ./run.sh --test
```

テストは 2 つに分かれている。`--test` はモデル層 (エンジンをリンクしないので速い)、
`--sync-test` は**モデルをエンジンに射影した結果**を見る。後者が見張っているのは
「ジェネレータ / リターン / マスターという 3 種類の持ち主を、ある場所では全部
列挙して別の場所では 2 つしか列挙していない」という種類のバグで、実際にそれで
出たバグが 3 件ある (マスターのエフェクトが保存されない・リターンのエフェクト
エディタが即閉じる・マスターの 4 個目のエフェクトが無言で消える)。

不変条件は `sync::findSyncProblems` 1 か所に書いてあり、**Debug ビルドでは同期の
たびに assert される**。つまり Debug で GUI を触っているだけでこの種のバグを
探していることになる。

対象は「モデル層の算術」「ドキュメントの編集操作（undo/redo・XML 往復を含む）」「ピアノロールのノート移動ジェスチャ」。エンジン（tracktion）と GUI コンポーネントは対象外で、テストバイナリは engine をリンクしない。フェッチを避けたいときは `-DCARVE_BUILD_TESTS=OFF`。

エンジンを通す検査は二つある。

- `carve-render --check-note-offs`（`tests/NoteOffPlayback.cpp`）。再生中にノート・クリップ・パターンを消して、鳴っていた音のノートオフが楽器に届くかを、ホスト型オーディオデバイスでブロック単位に回して確かめる。終了コードは鳴りっぱなしになったシナリオ数。
- `carve-render --check-signal-path`（`tests/SignalPathChecks.cpp`）。テスト信号 (インパルス・サイン・ピンクノイズ・ノイズバースト) をオーディオクリップで流し、書き出しと同じレンダーを通して元の信号と比べる。見ているのはフェーダー / マスター / グループ / センドのゲイン (±0.02dB)、2 系統の加算、両パン則、サンプルレート変換 (歪み -100dB 以下・15kHz で ±0.1dB・遅延 0)、ピッチシフターの遅延補償、挿入直後の Saturation / Distortion の音量、Compressor と EQ が効くこと、新規 Slate の音色と立ち上がり。どれもかつて壊れていた箇所で、各ケースのコメントに経緯がある。終了コードは失敗した項目数。音声経路を触ったら回す。

## CLI

```sh
BIN=./build/carve-render_artefacts/Release/carve-render
$BIN --demo demo.wav              # デモ曲をレンダリング
$BIN --sample                     # 付属サンプル曲の一覧
$BIN --sample "Neon Streets" a.wav  # サンプル曲をレンダリング (番号でも可)
$BIN --write-demo demo.carve      # デモ曲を .carve として書き出し
$BIN song.carve out.wav           # .carve をレンダリング
$BIN --scan                       # VST3/AU をスキャン (GUI と設定を共有)
$BIN --plugin-demo <名前> out.wav  # 名前でマッチした VSTi でデモ曲をレンダ
$BIN --rate 48000 song.carve out.wav  # レートを指定 (既定 44100)
$BIN song.carve out.ogg           # 拡張子 .ogg なら Ogg Vorbis (192 kbps)
$BIN --game-loop song.carve bgm.ogg  # 曲頭〜LOOP の終わり、LOOPSTART/LOOPLENGTH 付き
```

`--rate` はどのコマンドにも付けられる。既定は 44100 Hz 固定で、オーディオ
デバイスの設定には左右されない — GUI がデバイスを掴んでいるかどうかで
同じ曲が 44.1k/48k に振れると、md5 比較の回帰チェックが成立しないため。
ブロックサイズも同じ理由で固定 (512)。

## 配布 (署名と公証)

```sh
scripts/release.sh                 # ビルド → 署名 → 公証 → staple → dist/CarveDAW-<version>.dmg
scripts/release.sh --no-notarize   # 署名だけ試す
```

必要なものは Developer ID Application 証明書 (Apple Developer Program) と、
`xcrun notarytool store-credentials carve-notary ...` で保存した公証用の資格情報。
手順はスクリプト冒頭のコメントにある。Hardened Runtime で動かすので、他社の
AU / VST3 を読み込むための `disable-library-validation` だけを
`scripts/Carve.entitlements` で許可している。

## 実装メモ

- **プラグインスキャンは子プロセス**で走る。読み込みで落ちるプラグインが
  あってもアプリは巻き添えにならず、ブラックリスト入りして続行する
- **CLI レンダラーは決定的**: 同一曲のレンダリングはビット一致する
  (発振位相の乱数シードを固定する自前パッチ `patches/` + 単一スレッド実行
  + サンプルレートとブロックサイズをデバイスから切り離して固定)。
  GUI の再生と書き出しはマルチスレッドのまま
- **書き出しは手前から始めて頭を捨てる**: tracktion のレンダラは、グラフに
  遅延 (マスターの Limiter の先読みなど) があると最初のブロックだけ音を
  さらに遅延分遅らせて出し、次のブロックで正しい位置へ飛ぶ。曲頭で鳴る音が
  無音 → 遅れた音 → クリックになる (Renderer::Parameters に回避手段は無く、
  上流の develop も同じ)。そこで CLI もエクスポート画面も、書き出し範囲を
  0.5 秒以上 (ブロック単位) 手前から一時ファイルへレンダリングし、その分を
  削って書き出し先へ移す (`src/sync/RenderPreroll.h`)。遅延の無い曲の結果は
  ビット単位で変わらない
- オーディオクリップは `start` が拍 (音楽的)、`length`/`offset` が秒 (物理的)。
  テンポ変更で位置は動くが、音は伸び縮みしない

### オーディオ入力を開かない理由 (MIDI 入力は開く)

MIDI 録音はするが、オーディオは録音しない。`src/EngineSetup.h` はオーディオ入力を
開かないままにしてある (`shouldOpenAudioInputByDefault() == false`)。設定画面の
オーディオ選択も入力チャンネル 0 で作ってあるので、ここからも開かない。MIDI 入力は
別のドライバなのでこの話とは無関係で、`MidiInputController` が初めて見た物理 MIDI
入力を 1 度だけ有効にし、以後は設定画面のチェックに従う。

オーディオ入力を開かないのは、JUCE 8.0.6 の CoreAudio バックエンドにある
境界外書き込みを避けるためでもある:

`CoreAudioInternal::reopen()` はデインターリーブ用 temp バッファをデバイスの
実ブロックサイズで確保した後、`bufferSize` を要求値に上書きするが再確保しない。
要求サイズを受け付けないデバイス (例: Bluetooth ヘッドセットの 16kHz/320 フレーム
入力に 512 を要求) があると、コールバックがバッファを溢れてヒープを壊す。
既定の入出力が別デバイスだと JUCE が AudioIODeviceCombiner を作るため踏みやすい。

オーディオ入力を開くように変える場合は、`getAvailableBufferSizes()` から要求サイズを
選ぶか、JUCE 側にパッチを当てること。

## ライセンスの経緯

ソースと GitHub Releases は **AGPL-3.0-or-later**。全文は [LICENSE](LICENSE)。
依存関係は [NOTICE.md](NOTICE.md)。Mac App Store 向けの別ビルド (JUCE Starter と
Tracktion Engine Personal、売上の上限、`-DCARVE_APP_STORE=ON`) は
[LICENSING.md](LICENSING.md)。アプリが表示する第三者表記は
[THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt)。

土台はデュアルライセンスのまま: tracktion_engine 3.2.0 が GPL3 / 商用、その中の
JUCE 8.0.6 が AGPL3 / 商用。ソースと公証済み dmg はコピーレフト側で、いちばん強い
AGPL に揃う。App Store のバイナリだけ商用側で組む。リポジトリの LICENSE は変えない。

AGPL のネットワーク条項 (13条) はネットワーク越しに使わせるソフトの話で、本アプリは
何処とも通信しないデスクトップアプリなので、実際の義務は普通の GPL と同じ
「受け取った人がソースを手に入れられること」= このリポジトリ。

配布の本線は今も **GitHub Releases の公証済み .dmg** (`scripts/release.sh`)。
このバイナリは AGPL のままなので App Store には出さない。App Store 用は
`-DCARVE_APP_STORE=ON` の別ビルドで、VST3 ホストはそこで外れる。サンドボックスで
他人のプラグインを読む問題はライセンスとは別に残る。

同梱の AirWindows (189 種、`TRACKTION_AIR_WINDOWS` で有効化可) は同梱コピーに
ライセンス文が無いため未使用のまま。
