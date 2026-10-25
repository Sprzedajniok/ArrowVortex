"""Generates a song and a recording of it being played, for testing the video
sync of the viewer (ArrowVortex --video-sync).

The recording is what OBS would have recorded of an evening: menu music, the
song preview looping in the song select menu, a restart after 15 seconds, the
play the replay is of, the evaluation screen and a second play of the song.
The song is synthetic drums and notes, the simfile is a copy of the given one
that uses it, with a preview the recording loops.

usage: make_test_recording.py SIMFILE.sm OUT_DIR [--rate 1.25] [--seed 1]
       [--offset 92.0137] [--truth TRUTH.json]

OUT_DIR gets song/<simfile> with song/music.wav and three recordings:
"2026-10-22 19-58-30.wav", named like OBS names them, which 90 seconds after
its start is the replay date of make_test_replay.py, "game.wav" without the
second play, for finding the play without the date, and "nothing.wav" with
only the menus and the restart, in which the play must not be found. Where
song time 0 is in the first two is printed and written to TRUTH.json if
given, see check_video_sync.py.
"""
import argparse
import json
import math
import os
import random
import re
import struct
import wave

SONG_FREQUENCY = 22050
RECORDING_FREQUENCY = 24000
PREVIEW_START, PREVIEW_LENGTH = 20.0, 8.0


def make_music(rnd, length, bpm, frequency):
    """Drums and notes, with a different pattern every bar like music has."""
    out = [0.0] * int(length * frequency)

    def add(start, samples):
        i0 = int(start * frequency)
        for k, v in enumerate(samples):
            if 0 <= i0 + k < len(out):
                out[i0 + k] += v

    def kick():
        n = int(0.12 * frequency)
        return [0.8 * math.exp(-k / (0.03 * frequency)) *
                math.sin(2 * math.pi * (55 + 90 * math.exp(-k / (0.01 * frequency))) * k / frequency)
                for k in range(n)]

    def noise(length_s, decay_s, gain):
        n = int(length_s * frequency)
        return [gain * math.exp(-k / (decay_s * frequency)) * rnd.uniform(-1, 1)
                for k in range(n)]

    def tone(pitch, length_s, gain):
        n = int(length_s * frequency)
        f = 220.0 * 2 ** (pitch / 12.0)
        return [gain * min(1.0, k / 40.0) * math.exp(-k / (0.15 * frequency)) *
                math.sin(2 * math.pi * f * k / frequency) for k in range(n)]

    beat = 60.0 / bpm
    kick_samples = kick()
    t = 0.5
    while t < length - 1.0:
        for b in range(4):
            add(t + b * beat, kick_samples)
        for s in range(16):
            if rnd.random() < 0.45:
                add(t + s * beat / 4, noise(0.04, 0.008, 0.25))
        for s in (4, 12) if rnd.random() < 0.8 else (4, 10, 12):
            add(t + s * beat / 4, noise(0.15, 0.04, 0.5))
        for s in range(8):
            if rnd.random() < 0.6:
                add(t + s * beat / 2, tone(rnd.randrange(0, 24), 0.3, 0.3))
        t += 4 * beat
    return out


def write_wav(path, samples, frequency):
    peak = max(1e-9, max(abs(v) for v in samples))
    scale = 0.9 * 32767 / peak
    with wave.open(path, 'wb') as f:
        f.setnchannels(1)
        f.setsampwidth(2)
        f.setframerate(frequency)
        f.writeframes(b''.join(struct.pack('<h', int(v * scale)) for v in samples))


class Recording:
    def __init__(self, length, rnd):
        self.out = [rnd.gauss(0.0, 0.01) for _ in range(int(length * RECORDING_FREQUENCY))]

    def play(self, music, music_frequency, at, begin, end, rate, gain, fade=0.0):
        """Plays music from begin to end seconds at the rate, with music time
        `begin` at recording seconds `at`."""
        i0 = max(0, math.ceil(at * RECORDING_FREQUENCY))
        i1 = min(len(self.out), int((at + (end - begin) / rate) * RECORDING_FREQUENCY))
        for i in range(i0, i1):
            t = i / RECORDING_FREQUENCY - at
            pos = (begin + t * rate) * music_frequency
            k = int(pos)
            if k + 1 >= len(music):
                break
            v = music[k] + (music[k + 1] - music[k]) * (pos - k)
            g = gain
            if fade > 0.0:
                g *= min(1.0, t / fade, ((end - begin) / rate - t) / fade)
            self.out[i] += g * v


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('simfile')
    ap.add_argument('out_dir')
    ap.add_argument('--rate', type=float, default=1.25)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--offset', type=float, default=92.0137,
                    help='recording seconds at song time 0 of the replayed play')
    ap.add_argument('--length', type=float, default=62.0, help='song length')
    ap.add_argument('--truth')
    args = ap.parse_args()

    rnd = random.Random(args.seed)
    rate = args.rate
    song = make_music(rnd, args.length, 150.0, SONG_FREQUENCY)
    menu = make_music(rnd, 12.0, 128.0, SONG_FREQUENCY)

    song_dir = os.path.join(args.out_dir, 'song')
    os.makedirs(song_dir, exist_ok=True)
    write_wav(os.path.join(song_dir, 'music.wav'), song, SONG_FREQUENCY)
    text = open(args.simfile, encoding='utf-8').read()
    text = re.sub(r'#MUSIC:[^;]*;', '#MUSIC:music.wav;', text)
    text = re.sub(r'#SAMPLESTART:[^;]*;\s*', '', text)
    text = re.sub(r'#SAMPLELENGTH:[^;]*;\s*', '', text)
    text = text.replace('#MUSIC:music.wav;', '#MUSIC:music.wav;\n#SAMPLESTART:%.3f;\n'
                        '#SAMPLELENGTH:%.3f;' % (PREVIEW_START, PREVIEW_LENGTH))
    with open(os.path.join(song_dir, os.path.basename(args.simfile)), 'w',
              encoding='utf-8', newline='\n') as f:
        f.write(text)

    play_seconds = args.length / rate
    second_play = args.offset + play_seconds + 30.0
    for name, plays in (('2026-10-22 19-58-30.wav', 2), ('game.wav', 1),
                        ('nothing.wav', 0)):
        rec = Recording((second_play if plays == 2 else args.offset) + play_seconds + 5.0,
                        random.Random(args.seed + 1))
        rec.play(menu, SONG_FREQUENCY, 0.0, 0.0, 10.0, 1.0, 0.4)
        for loop in range(3):
            rec.play(song, SONG_FREQUENCY, 10.0 + loop * (PREVIEW_LENGTH / rate + 1.0),
                     PREVIEW_START, PREVIEW_START + PREVIEW_LENGTH, rate, 0.4, fade=0.5)
        rec.play(song, SONG_FREQUENCY, 36.0, 0.0, 15.0, rate, 0.5)
        if plays >= 1:
            rec.play(song, SONG_FREQUENCY, args.offset, 0.0, args.length, rate, 0.5)
        rec.play(menu, SONG_FREQUENCY, args.offset + play_seconds + 2.0, 0.0, 10.0, 1.0, 0.4)
        if plays == 2:
            rec.play(song, SONG_FREQUENCY, second_play, 0.0, args.length, rate, 0.5)
        write_wav(os.path.join(args.out_dir, name), rec.out, RECORDING_FREQUENCY)

    truth = {'offset': args.offset}
    print(truth)
    if args.truth:
        with open(args.truth, 'w', encoding='utf-8') as f:
            json.dump(truth, f, indent=1)


if __name__ == '__main__':
    main()
