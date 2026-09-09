# TODO

完了した項目は履歴にある (git log で追える)。ここには残作業と、
着手時に必要になる調査済みの事実だけを置く。

## 進行中バッチ (2026-09-09)

領域ごとに worktree を分けて並列に進める。担当ごとの項目:

### A. ピアノロール (PianoRollComponent / NoteGestures)
- [ ] 複数選択中の右クリックで「一つ消える」「全部消える」の 2 通りになる。後者が正しい。原因調査と修正
- [ ] 複数選択中のベロシティ編集を一括適用に
- [ ] ⌘A / Ctrl+A で全選択
- [ ] Snap のオン/オフを設定として保存
- [ ] 何かの修飾キーで Snap を一時的に無視
- [ ] クオンタイズの Swing のかかりが弱い
- [ ] 複数選択中の音長調整を「全部同じ長さ」ではなく相対 (同じだけ伸縮) に

### B. タイムライン (PlaylistComponent)
- [ ] Paint モード中の Shift+ドラッグ矩形選択が、パターン上から始めるとパターン選択に負ける

### C. Pattern モード (EditSync Audition / GeneratorController / TransportBar)
- [ ] 選択中の Generator だけでなく、全 Generator の「最後に選択したパターン」を同時に演奏

### D. 808 Drums (DrumSynthPlugin / DrumSynthEditor)
- [ ] Kick に Click パラメータ

### E. ドラムサンプラー (DrumPadGrid / GeneratorWindow / SamplerPlugin)
- [ ] 発音中のパッドを光らせる
- [ ] パッドごとの音量・ピッチ・長さの調節

### F. エフェクト (EffectSlotList / EffectParameterWindow / plugins)
- [ ] Compressor にリダクション量のメーター
- [ ] エフェクト一覧に Distortion が出てこない
- [ ] Saturation エフェクト

### G. アプリ全体 (Main / MainComponent / TransportBar)
- [ ] 起動直後は空プロジェクト
- [ ] macOS メニューバー
- [ ] Save ボタン

### H. オーディオクリップ (ClipPropertiesPanel / SongModel AudioClip)
- [ ] サンプルが見つからないときの検索・再リンクダイアログ
- [ ] オーディオクリップ選択中はクリッププロパティパネルでオーディオのプロパティを編集、リロードも

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
