# TODO

完了した項目は履歴にある (git log で追える)。ここには残作業と、
着手時に必要になる調査済みの事実だけを置く。

## 2026-09-09 バッチの残り

worktree 並列は重すぎたので main で直列に取り込んだ。20 件中 18 件は済み
(git log)。残り:

- [ ] ドラムサンプラー: パッドごとの音量・ピッチ・長さ
      tracktion の SamplerPlugin は sound ごとに gain/pan/pitch
      (setSoundParams)、開始・長さ (setSoundExcerpt)、open-ended を持つ。
      モデル (SamplerSound) に持たせて EditSync で compare-before-set、
      Inst タブでパッド選択時にノブを出す (KnobPanel.h)
- [ ] オーディオクリップ選択中はクリッププロパティパネルでオーディオの
      プロパティ (ファイル・開始・長さ) を編集、リロード / Relocate ボタン
      syncAudioClips はクリップを id で保持するので、リロードは配置側に
      再読込カウンタを持たせるか EditSync::reloadAudioClip(id) を足す

## 残作業


- [ ] MIDI キーボード入力
      今は音のプレビューのみで、弾くことも録ることもできない。
      無効化してあるのはオーディオ入力で MIDI 入力は別系統なので、
      EngineSetup.h の CoreAudio バグ回避 (README 参照) には当たらない
- [ ] モディファイアの編集 UI (エンジンとモデルは実装済み・.carve 直書きで動く。
      LFO の rate/depth/wave と ASSIGN を編集するパネルが無い)
- [ ] オートメーションのカーブベンド (PT.curve は保存・再生されるが
      レーン UI は直線描画のみ)
- [ ] オーディオ録音
      着手前に README の「録音に着手するときの注意」を読むこと
