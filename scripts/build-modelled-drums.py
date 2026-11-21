#!/usr/bin/env python3
"""Builds assets/instruments/modelled-drums/ for the Modelled Drums generator from the
PhysKit project (the physically modelled kit, instruments/drums/kit):

  - the five real-time models (player/models/*.bin.gz: PKD2 for the toms and the kick,
    PKS1 for the snare), copied as they are; the plugin gunzips them at load;
  - the cymbals: the same subset of the dry sample kit (out/PhysKitDry) as the kit's
    tools/rt_cymbals.py takes for player/live.html (its velocity layers, two round
    robins, tails cut 70 dB below the instrument's loudest sample with a 50 ms fade),
    as Ogg Vorbis instead of Opus (JUCE decodes Vorbis), and cymbals.txt, one line per
    sample: articulation layer layers rr group off_by file.

  python3 scripts/build-modelled-drums.py /path/to/instruments/drums/kit

Needs numpy and ffmpeg with libvorbis."""
import json, os, shutil, subprocess, sys, wave

import numpy as np

KIT = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else '../vibe/instruments/drums/kit')
OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'assets', 'instruments', 'modelled-drums')
KEEP = {  # articulation: kit layers kept (tools/rt_cymbals.py)
    'crash': [1, 3, 4, 6], 'crash_choke': [1, 3], 'ride': [1, 4, 6], 'ride_bell': [1, 2, 4],
    'hh_closed': [1, 3, 4, 6], 'hh_open': [1, 3, 4], 'hh_pedal': [1, 3]}
RR, FLOOR_DB, QUALITY = 2, -70, 6

os.makedirs(OUT, exist_ok=True)
for f in os.listdir(OUT):
    os.remove(os.path.join(OUT, f))

for name in ('tom_hi', 'tom_mid', 'tom_floor', 'snare', 'kick'):
    shutil.copy(os.path.join(KIT, 'player', 'models', name + '.bin.gz'), os.path.join(OUT, name + '.bin.gz'))


def read24(path):
    with wave.open(path) as w:
        n, ch, sw, sr = w.getnframes(), w.getnchannels(), w.getsampwidth(), w.getframerate()
        raw = np.frombuffer(w.readframes(n), dtype=np.uint8)
    assert sw == 3
    b = raw.reshape(-1, 3).astype(np.int32)
    x = (b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16))
    x = np.where(x >= 1 << 23, x - (1 << 24), x).astype(np.float32) / 8388608
    return x.reshape(-1, ch), sr


meta = json.load(open(os.path.join(KIT, 'out', 'PhysKitDry', 'samples.json')))
lines, total = [], 0
for art, layers in KEEP.items():
    sel = [m for m in meta if m['articulation'] == art and m['layer'] in layers and m['rr'] <= RR]
    data = {m['file']: read24(os.path.join(KIT, 'out', 'PhysKitDry', m['file'])) for m in sel}
    peak = max(float(np.abs(x).max()) for x, _ in data.values())
    floor = peak * 10 ** (FLOOR_DB / 20)
    for m in sel:
        x, sr = data[m['file']]
        loud = np.nonzero(np.abs(x).max(axis=1) > floor)[0]
        fade = int(0.05 * sr)
        n = min(len(x), (int(loud[-1]) if len(loud) else 0) + fade)
        y = x[:n].copy()
        y[n - fade:] *= ((np.arange(fade, 0, -1) - 1) / fade)[:, None] ** 2
        layer = layers.index(m['layer']) + 1
        name = f"{art}_l{layer}_rr{m['rr']}.ogg"
        subprocess.run(['ffmpeg', '-v', 'error', '-y', '-f', 'f32le', '-ar', str(sr), '-ac', str(y.shape[1]), '-i', '-',
                        '-c:a', 'libvorbis', '-q:a', str(QUALITY), os.path.join(OUT, name)],
                       input=np.ascontiguousarray(y).tobytes(), check=True)
        total += os.path.getsize(os.path.join(OUT, name))
        lines.append(f"{art} {layer} {len(layers)} {m['rr']} {m['group']} {m.get('off_by') or 0} {name}")

open(os.path.join(OUT, 'cymbals.txt'), 'w').write('\n'.join(lines) + '\n')
print(f'{len(lines)} cymbal samples, {total / 1e6:.2f} MB -> {OUT}')
