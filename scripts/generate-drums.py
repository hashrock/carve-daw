#!/usr/bin/env python3
"""Renders the factory drum kit that ships in content/Drums.

The samples are synthesised rather than recorded so that everything Carve
ships is its own: no sample licence to honour, and the kit can be changed by
editing this file instead of by finding new source material.

    python3 scripts/generate-drums.py

Deterministic -- the noise is seeded -- so a re-run of an unchanged script
rewrites the same bytes and shows up as no diff at all.
"""

import os
import struct
import wave

import numpy as np

RATE = 44100
OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "content", "Drums")


def seconds(n):
    return np.arange(int(RATE * n)) / RATE


def decay(t, seconds_to_silence, curve=1.0):
    """An exponential fall to roughly -60dB at `seconds_to_silence`."""
    return np.exp(-t * (6.9 / seconds_to_silence)) ** curve


def noise(t, seed):
    return np.random.default_rng(seed).uniform(-1.0, 1.0, len(t))


def one_pole_high(x, cutoff):
    """A single-pole high pass, written out because the kit is worth no more
    than numpy as a dependency."""
    a = np.exp(-2.0 * np.pi * cutoff / RATE)
    low = np.zeros_like(x)
    acc = 0.0
    for i, sample in enumerate(x):
        acc = a * acc + (1.0 - a) * sample
        low[i] = acc
    return x - low


def one_pole_low(x, cutoff, poles=1):
    a = np.exp(-2.0 * np.pi * cutoff / RATE)

    for _ in range(poles):
        out = np.zeros_like(x)
        acc = 0.0
        for i, sample in enumerate(x):
            acc = a * acc + (1.0 - a) * sample
            out[i] = acc
        x = out

    return x


def sweep(t, start_hz, end_hz, fall):
    """A sine whose pitch falls from start to end -- the body of every drum
    here that has a pitch at all."""
    f = end_hz + (start_hz - end_hz) * np.exp(-t / fall)
    return np.sin(2.0 * np.pi * np.cumsum(f) / RATE)


def kick():
    t = seconds(0.6)
    body = sweep(t, 150.0, 47.0, 0.028) * decay(t, 0.45)
    click = one_pole_high(noise(t, 1), 3000.0) * decay(t, 0.006)
    return body + 0.35 * click


def snare():
    t = seconds(0.35)
    body = (np.sin(2 * np.pi * 185 * t) + 0.7 * np.sin(2 * np.pi * 330 * t)) * decay(t, 0.14)
    # Two poles off the top: one is 6dB an octave, which leaves a snare
    # sounding like the tape hiss it is made of rather than like a drum.
    rattle = one_pole_low(one_pole_high(noise(t, 2), 1200.0), 7500.0, poles=2) * decay(t, 0.19)
    return 0.55 * body + 0.8 * rattle


def rim():
    t = seconds(0.09)
    tone = (np.sin(2 * np.pi * 1700 * t) + np.sin(2 * np.pi * 480 * t)) * decay(t, 0.03)
    return 0.6 * tone + 0.5 * one_pole_high(noise(t, 3), 2500.0) * decay(t, 0.012)


def clap():
    t = seconds(0.42)
    out = np.zeros_like(t)
    body = one_pole_high(one_pole_low(noise(t, 4), 4000.0, poles=2), 900.0)

    # Three slaps a few milliseconds apart, then the room behind them: what
    # makes a clap a clap rather than a noise burst.
    for offset, level in ((0.000, 1.0), (0.011, 0.85), (0.021, 0.7)):
        start = int(offset * RATE)
        env = decay(t[: len(t) - start], 0.012)
        out[start:] += level * body[: len(t) - start] * env

    return out + 0.45 * body * decay(t, 0.24)


def closed_hat():
    t = seconds(0.12)
    return one_pole_low(one_pole_high(noise(t, 5), 6000.0), 10000.0, poles=2) * decay(t, 0.035)


def open_hat():
    t = seconds(0.7)
    return one_pole_low(one_pole_high(noise(t, 6), 5500.0), 10000.0, poles=2) * decay(t, 0.42)


def tom(pitch, seed, length):
    t = seconds(length)
    body = sweep(t, pitch * 1.6, pitch, 0.05) * decay(t, length * 0.7)
    return body + 0.18 * one_pole_high(noise(t, seed), 2000.0) * decay(t, 0.01)


def crash():
    t = seconds(1.6)
    metal = sum(np.sin(2 * np.pi * f * t) for f in (3120, 4360, 5410, 7230)) / 4.0
    wash = one_pole_low(one_pole_high(noise(t, 9), 5000.0), 11000.0)
    return (0.35 * metal + wash) * decay(t, 1.3, curve=0.8)


KIT = [
    ("Kick", kick),
    ("Snare", snare),
    ("Rim", rim),
    ("Clap", clap),
    ("Closed Hat", closed_hat),
    ("Open Hat", open_hat),
    ("Tom Low", lambda: tom(110.0, 7, 0.45)),
    ("Tom High", lambda: tom(190.0, 8, 0.35)),
    ("Crash", crash),
]


def write(path, samples):
    # Normalised to -1dBFS: every pad on the same footing, and headroom left
    # for whatever the kit is played through.
    peak = float(np.abs(samples).max())
    samples = samples / peak * 0.891

    # A short fade out, so a sample that is still moving at its last frame
    # does not click when it ends.
    fade = min(256, len(samples))
    samples[-fade:] *= np.linspace(1.0, 0.0, fade)

    frames = np.clip(np.round(samples * 32767.0), -32768, 32767).astype("<i2")

    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(frames.tobytes())


def main():
    os.makedirs(OUT, exist_ok=True)

    for name, build in KIT:
        path = os.path.join(OUT, name + ".wav")
        write(path, build())
        print(f"{os.path.getsize(path):8d}  {path}")


if __name__ == "__main__":
    main()
