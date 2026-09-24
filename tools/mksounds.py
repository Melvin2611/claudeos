#!/usr/bin/env python3
"""Synthesize the ClaudeOS system sounds and a demo song (16-bit PCM WAV)."""
import math, os, random, struct, sys, wave

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
SYS = os.path.join(ROOT, "assets", "sounds")
MUSIC = os.path.join(ROOT, "rootfs", "home", "Music")
os.makedirs(SYS, exist_ok=True)
os.makedirs(MUSIC, exist_ok=True)


def write(path, samples, rate, channels=1):
    with wave.open(path, "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b"".join(struct.pack("<h", max(-32767, min(32767, int(s * 32767)))) for s in samples))
    print("wrote", os.path.relpath(path, ROOT), os.path.getsize(path))


def note_freq(n):
    return 440.0 * 2 ** ((n - 69) / 12)


def tone(freq, dur, rate, vol=0.5, attack=0.01, decay=0.3, shape="sine"):
    n = int(dur * rate)
    out = []
    for i in range(n):
        t = i / rate
        env = min(1.0, t / attack) * math.exp(-t / decay) if decay else min(1.0, t / attack)
        ph = 2 * math.pi * freq * t
        if shape == "sine":
            v = math.sin(ph) + 0.25 * math.sin(2 * ph) + 0.1 * math.sin(3 * ph)
        elif shape == "square":
            v = 0.6 if math.sin(ph) > 0 else -0.6
        elif shape == "tri":
            v = 2 / math.pi * math.asin(math.sin(ph))
        else:
            v = math.sin(ph)
        out.append(v * env * vol)
    return out


def mix(dst, src, at):
    for i, s in enumerate(src):
        if at + i < len(dst):
            dst[at + i] += s


def startup():
    rate = 44100
    buf = [0.0] * int(rate * 2.2)
    for k, (n, t) in enumerate([(60, 0.0), (64, 0.12), (67, 0.24), (72, 0.36)]):
        mix(buf, tone(note_freq(n), 1.8 - t, rate, 0.22, 0.02, 0.7), int(t * rate))
    mix(buf, tone(note_freq(48), 1.8, rate, 0.18, 0.05, 0.9), 0)
    write(os.path.join(SYS, "startup.wav"), buf, rate)


def notify():
    rate = 44100
    buf = [0.0] * int(rate * 0.6)
    mix(buf, tone(note_freq(81), 0.5, rate, 0.25, 0.005, 0.12), 0)
    mix(buf, tone(note_freq(88), 0.45, rate, 0.25, 0.005, 0.15), int(0.09 * rate))
    write(os.path.join(SYS, "notify.wav"), buf, rate)


def error():
    rate = 44100
    buf = [0.0] * int(rate * 0.5)
    mix(buf, tone(note_freq(57), 0.45, rate, 0.3, 0.005, 0.15, "tri"), 0)
    mix(buf, tone(note_freq(56), 0.4, rate, 0.2, 0.005, 0.15, "tri"), int(0.05 * rate))
    write(os.path.join(SYS, "error.wav"), buf, rate)


def shutdown():
    rate = 44100
    buf = [0.0] * int(rate * 1.4)
    for k, n in enumerate([72, 67, 64, 60]):
        mix(buf, tone(note_freq(n), 1.0, rate, 0.2, 0.01, 0.4), int(k * 0.12 * rate))
    write(os.path.join(SYS, "shutdown.wav"), buf, rate)


def song():
    """A small 30 second tune: chords, bass, melody and a hi-hat."""
    rate = 22050
    bpm = 112
    beat = 60 / bpm
    bars = 14
    length = bars * 4 * beat
    left = [0.0] * int(rate * (length + 1.5))
    right = [0.0] * len(left)
    prog = [(57, "m"), (53, ""), (48, ""), (55, "")]    # Am F C G
    random.seed(11)
    melody_scale = [69, 71, 72, 74, 76, 77, 79, 81]
    for bar in range(bars):
        root, kind = prog[bar % 4]
        t0 = bar * 4 * beat
        third = 3 if kind == "m" else 4
        chord = [root, root + third, root + 7]
        for n in chord:
            c = tone(note_freq(n), 4 * beat, rate, 0.07, 0.08, 1.8, "tri")
            mix(left, c, int(t0 * rate))
            mix(right, c, int(t0 * rate))
        for b in range(4):
            bass = tone(note_freq(root - 12), beat * 0.9, rate, 0.22, 0.01, 0.35, "sine")
            mix(left, bass, int((t0 + b * beat) * rate))
            mix(right, bass, int((t0 + b * beat) * rate))
            # hi-hat: short noise burst
            hat = [random.uniform(-1, 1) * 0.05 * math.exp(-i / (rate * 0.02)) for i in range(int(rate * 0.06))]
            mix(left, hat, int((t0 + b * beat + beat / 2) * rate))
            mix(right, hat, int((t0 + b * beat + beat / 2) * rate))
        if bar >= 2:
            pos = 0.0
            while pos < 4 * beat - 0.01:
                d = random.choice([beat / 2, beat / 2, beat, beat * 1.5])
                d = min(d, 4 * beat - pos)
                n = random.choice(melody_scale)
                if random.random() < 0.15:
                    pos += d
                    continue
                m = tone(note_freq(n), d * 0.95, rate, 0.16, 0.01, 0.5, "sine")
                pan = 0.35 + 0.3 * random.random()
                mix(left, [s * (1 - pan) * 1.4 for s in m], int((t0 + pos) * rate))
                mix(right, [s * pan * 1.4 for s in m], int((t0 + pos) * rate))
                pos += d
    # fade out
    fade = int(rate * 2)
    for i in range(fade):
        g = 1 - i / fade
        left[-fade + i] *= g
        right[-fade + i] *= g
    inter = []
    for l, r in zip(left, right):
        inter += [l, r]
    write(os.path.join(MUSIC, "Claude-Groove.wav"), inter, rate, 2)


startup()
notify()
error()
shutdown()
song()
