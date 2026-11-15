#!/usr/bin/env python3
"""
Synthesizes placeholder ambience for the sandbox (replace them with real
recordings any time; the scene only refers to the file names):

  sandbox/assets/audio/ambience/crickets_loop.wav      night insects
  sandbox/assets/audio/ambience/frogs_loop.wav         swamp frogs
  sandbox/assets/audio/ambience/day_insects_loop.wav   cicada-like daytime buzz
  sandbox/assets/audio/ambience/bird_1..4.wav          bird calls (one-shots)
  sandbox/assets/audio/ambience/hyena_1.wav            a distant whoop (one-shot)

Standard library only. Run from anywhere:
    python3 tools/generate_ambience.py
"""
import math
import os
import random
import struct
import wave

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "sandbox", "assets", "audio", "ambience")
RATE = 22050
TAU = 2.0 * math.pi


def write_wav(name, samples, peak=0.9):
    os.makedirs(ROOT, exist_ok=True)
    m = max(1e-6, max(abs(s) for s in samples))
    k = peak / m
    with wave.open(os.path.join(ROOT, name), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<h", int(max(-1.0, min(1.0, s * k)) * 32000)) for s in samples))
    print("wrote", name, f"{len(samples) / RATE:.1f} s")


def loop_fade(samples, fade):
    """Crossfade the end into the start so the loop is seamless."""
    n = len(samples)
    res = samples[: n - fade]
    for i in range(fade):
        t = i / fade
        res[i] = res[i] * t + samples[n - fade + i] * (1 - t)
    return res


def add_tone(buf, start, dur, freq_fn, amp_fn, harmonics=((1, 1.0),)):
    """Adds a tone with time-varying frequency/amplitude (phase-continuous)."""
    phase = 0.0
    n = int(dur * RATE)
    for i in range(n):
        j = start + i
        if j >= len(buf):
            break
        t = i / n
        phase += TAU * freq_fn(t) / RATE
        a = amp_fn(t)
        if a <= 0.0:
            continue
        buf[j] += a * sum(h_amp * math.sin(phase * h) for h, h_amp in harmonics)


def lowpass(samples, k):
    lp, out = 0.0, []
    for s in samples:
        lp += (s - lp) * k
        out.append(lp)
    return out


def crickets(rnd):
    seconds = 17.0
    buf = [0.0] * int(seconds * RATE)
    for c in range(7):
        carrier = rnd.uniform(3600, 4800)
        period = rnd.uniform(0.32, 0.85)
        pulses = rnd.randint(2, 4)
        amp = rnd.uniform(0.05, 0.25)
        t = rnd.uniform(0, period)
        while t < seconds - 0.2:
            for p in range(pulses):
                add_tone(buf, int((t + p * 0.03) * RATE), 0.018, lambda _t, f=carrier: f,
                         lambda x, a=amp: a * math.sin(math.pi * x) ** 2, ((1, 1.0), (2, 0.15)))
            t += period * rnd.uniform(0.95, 1.05)
    # A faint, steady bed of distant insects.
    hp = 0.0
    for i in range(len(buf)):
        w = rnd.uniform(-1, 1)
        buf[i] += (w - hp) * 0.012 * (1.0 + 0.3 * math.sin(i / RATE * 0.7))
        hp = w
    write_wav("crickets_loop.wav", loop_fade(buf, RATE // 2), 0.6)


def frogs(rnd):
    seconds = 18.0
    buf = [0.0] * int(seconds * RATE)
    for f in range(6):
        base = rnd.uniform(220, 520)
        pulse_rate = rnd.uniform(22, 40)
        dur = rnd.uniform(0.12, 0.3)
        amp = rnd.uniform(0.15, 0.4)
        t = rnd.uniform(0, 2)
        while t < seconds - 0.6:
            for rep in range(rnd.choice((1, 1, 2, 3))):
                start = t + rep * (dur + 0.07)
                add_tone(buf, int(start * RATE), dur, lambda x, b=base: b * (1.0 + 0.08 * x),
                         lambda x, a=amp, pr=pulse_rate, d=dur: a * math.sin(math.pi * x) * max(0.0, math.sin(TAU * pr * x * d)) ** 2,
                         ((1, 1.0), (2, 0.5), (3, 0.3), (5, 0.12)))
            t += rnd.uniform(1.2, 3.5)
    write_wav("frogs_loop.wav", loop_fade(lowpass(buf, 0.35), RATE // 2), 0.6)


def day_insects(rnd):
    seconds = 16.0
    n = int(seconds * RATE)
    buf = [0.0] * n
    hp = 0.0
    for i in range(n):
        t = i / RATE
        swell = 0.5 + 0.5 * math.sin(TAU * t / 8.0) * math.sin(TAU * t / 5.3 + 1.0)
        buzz = max(0.0, math.sin(TAU * 140.0 * t)) ** 4  # fast tymbal clicks
        w = rnd.uniform(-1, 1)
        noise = w - hp
        hp = w
        buf[i] = (0.6 * math.sin(TAU * 5200.0 * t) + 0.4 * noise) * buzz * (0.15 + 0.85 * swell)
    write_wav("day_insects_loop.wav", loop_fade(buf, RATE // 2), 0.35)


def birds(rnd):
    # 1: two-note whistle, high then lower.
    buf = [0.0] * int(0.75 * RATE)
    add_tone(buf, 0, 0.26, lambda x: 3300 - 300 * x, lambda x: math.sin(math.pi * x) ** 0.7)
    add_tone(buf, int(0.36 * RATE), 0.32, lambda x: 2700 - 250 * x, lambda x: math.sin(math.pi * x) ** 0.7)
    write_wav("bird_1.wav", buf, 0.7)
    # 2: a trill.
    buf = [0.0] * int(0.8 * RATE)
    add_tone(buf, 0, 0.7, lambda x: 2600 + 450 * math.sin(TAU * 18 * x * 0.7), lambda x: math.sin(math.pi * x) * (0.6 + 0.4 * math.sin(TAU * 9 * x)))
    write_wav("bird_2.wav", buf, 0.6)
    # 3: quick rising chirps.
    buf = [0.0] * int(0.7 * RATE)
    for k in range(4):
        add_tone(buf, int(k * 0.14 * RATE), 0.07, lambda x: 2000 + 2200 * x, lambda x: math.sin(math.pi * x))
    write_wav("bird_3.wav", buf, 0.6)
    # 4: a dove's soft "coo-COO-coo".
    buf = [0.0] * int(1.5 * RATE)
    for start, dur, amp in ((0.0, 0.28, 0.6), (0.38, 0.45, 1.0), (0.95, 0.3, 0.5)):
        add_tone(buf, int(start * RATE), dur, lambda x: 470 - 60 * x, lambda x, a=amp: a * math.sin(math.pi * x) ** 1.5,
                 ((1, 1.0), (2, 0.25), (3, 0.08)))
    write_wav("bird_4.wav", lowpass(buf, 0.3), 0.7)


def hyena(rnd):
    # A rising "whoo-oop", softened as if far away.
    buf = [0.0] * int(1.6 * RATE)
    add_tone(buf, 0, 1.3, lambda x: 320 + 650 * x ** 1.6, lambda x: math.sin(math.pi * x) ** 0.6 * (1.0 - 0.3 * x),
             ((1, 1.0), (2, 0.35), (3, 0.12)))
    write_wav("hyena_1.wav", lowpass(buf, 0.18), 0.7)


if __name__ == "__main__":
    rnd = random.Random(2024)
    crickets(rnd)
    frogs(rnd)
    day_insects(rnd)
    birds(rnd)
    hyena(rnd)
