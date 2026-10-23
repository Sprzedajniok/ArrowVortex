"""Generates a synthetic ArrowReplay file for a .sm chart, for testing the viewer.

It plays the chart like a slightly sloppy player would and writes the events
the ArrowReplay Simply Love module would have recorded, judged with ITG
windows. Only constant-BPM-segment charts without warps are supported.

usage: make_test_replay.py SIMFILE.sm VIRTUAL_PATH OUT.avreplay
       [--difficulty Challenge] [--rate 1.0] [--global-offset 0.0] [--seed 1]
       [--truth TRUTH.json]

The judgment counts it generated are printed, and written to TRUTH.json if
given, see check_report.py.
"""
import argparse
import json
import random

ROWS = 48
WINDOWS = [0.0215, 0.043, 0.102, 0.135, 0.180]  # W1..W5
ADD = 0.0015
W0 = 0.0135 + ADD
HOLD_WINDOW = 0.32 + ADD
MINE_WINDOW = 0.07 + ADD


def parse_pairs(value):
    out = []
    for item in value.split(','):
        item = item.strip()
        if '=' in item:
            beat, val = item.split('=')[:2]
            out.append((float(beat), float(val)))
    return sorted(out)


def parse_sm(path, difficulty):
    text = open(path, encoding='utf-8', errors='replace').read()
    tags = {}
    charts = []
    for part in text.split('#')[1:]:
        if ':' not in part:
            continue
        key, value = part.split(':', 1)
        value = value.split(';')[0]
        key = key.strip().upper()
        if key == 'NOTES':
            fields = value.split(':')
            charts.append([f.strip() for f in fields[:5]] + [fields[5]])
        else:
            tags[key] = value
    chart = next(c for c in charts if c[0] == 'dance-single' and
                 c[2].lower() == difficulty.lower())
    return tags, chart


def beat_to_time(beat, offset, bpms, stops):
    time = -offset
    for i, (b, bpm) in enumerate(bpms):
        end = bpms[i + 1][0] if i + 1 < len(bpms) else float('inf')
        if beat <= b:
            break
        time += (min(beat, end) - b) * 60.0 / bpm
    for b, length in stops:
        if b < beat:
            time += length
    return time


def parse_notes(body):
    notes = []  # (beat, col, kind, end_beat)
    open_holds = {}
    for m, measure in enumerate(body.split(',')):
        rows = [r.strip() for r in measure.split('\n') if r.strip() and not r.strip().startswith('//')]
        for i, row in enumerate(rows):
            beat = m * 4 + i * 4.0 / len(rows)
            for col, ch in enumerate(row[:4]):
                if ch in '1L':
                    notes.append([beat, col, 'lift' if ch == 'L' else 'tap', beat])
                elif ch in '24':
                    open_holds[col] = len(notes)
                    notes.append([beat, col, 'roll' if ch == '4' else 'hold', beat])
                elif ch == '3' and col in open_holds:
                    notes[open_holds.pop(col)][3] = beat
                elif ch == 'M':
                    notes.append([beat, col, 'mine', beat])
    return notes


def judge(offset):
    for i, w in enumerate(WINDOWS):
        if abs(offset) <= w + ADD:
            return 'W%d' % (i + 1)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('simfile')
    ap.add_argument('virtual_path')
    ap.add_argument('out')
    ap.add_argument('--difficulty', default='Challenge')
    ap.add_argument('--rate', type=float, default=1.0)
    ap.add_argument('--global-offset', type=float, default=0.0)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--truth')
    args = ap.parse_args()

    rnd = random.Random(args.seed)
    rate, g = args.rate, args.global_offset
    tags, chart = parse_sm(args.simfile, args.difficulty)
    offset = float(tags.get('OFFSET', '0') or 0)
    bpms = parse_pairs(tags.get('BPMS', '0=120'))
    stops = parse_pairs(tags.get('STOPS', ''))

    notes = []
    for beat, col, kind, end in parse_notes(chart[5]):
        notes.append({'col': col, 'kind': kind,
                      'time': beat_to_time(beat, offset, bpms, stops),
                      'end': beat_to_time(end, offset, bpms, stops)})
    notes.sort(key=lambda n: (n['time'], n['col']))

    # Engine music seconds = chart time - rate * global offset.
    to_engine = lambda c: c - rate * g
    record_bias = 0.003  # recorder sees presses this much earlier than the engine

    events = []  # (time, line)
    truth = {'W1': 0, 'W2': 0, 'W3': 0, 'W4': 0, 'W5': 0, 'Miss': 0,
             'stray': 0, 'letgo': 0, 'mine': 0, 'rescored': 0}

    def press(col, chart_time, hold_len=0.06):
        t = to_engine(chart_time) - record_bias
        events.append((t, 'P\t%.6f\t%d\t1' % (t, col + 1)))
        events.append((t + hold_len, 'P\t%.6f\t%d\t0' % (t + hold_len, col + 1)))

    rows = {}
    for n in notes:
        if n['kind'] != 'mine':
            rows.setdefault(round(n['time'], 6), []).append(n)

    for row_time, row in sorted(rows.items()):
        results = []
        for n in row:
            col = n['col']
            if rnd.random() < 0.02:
                results.append((n, 'Miss', 0.0, 'None', 0.0, None))
                continue
            off = rnd.gauss(-0.004, 0.014)
            if rnd.random() < 0.03:
                off = rnd.choice([-1, 1]) * rnd.uniform(0.11, 0.17)
            early = ('None', 0.0)
            if len(row) == 1 and rnd.random() < 0.02:
                # Early decent, then a better press rescores it.
                eoff = -rnd.uniform(0.104, 0.134)
                press(col, n['time'] + eoff * rate, 0.03)
                early = ('W4', eoff)
                truth['rescored'] += 1
                off = rnd.gauss(0.0, 0.01)
            tns = judge(off) or 'Miss'
            hold_len = (n['end'] - n['time']) / rate + 0.05 if n['kind'] in ('hold', 'roll') else 0.06
            let_go = n['kind'] in ('hold', 'roll') and rnd.random() < 0.15
            if let_go:
                hold_len *= rnd.uniform(0.2, 0.7)
            press_time = n['time'] + off * rate
            press(col, press_time, hold_len * rate)
            results.append((n, tns, off, early[0], early[1], (press_time, hold_len, let_go)))

        # Rows are judged together once the last note of the row is judged.
        hit_times = [to_engine(r[5][0]) for r in results if r[5] and r[1] != 'Miss']
        if any(r[1] == 'Miss' for r in results):
            t = to_engine(row_time) + (WINDOWS[-1] + ADD) * rate + 0.004
        else:
            t = max(hit_times)
        for n, tns, off, etns, eoff, info in results:
            truth[tns] += 1
            if tns == 'Miss':
                off = 0.0
            events.append((t, 'T\t%.6f\t%d\t%s\t%.6f\t%s\t%.6f\t0\t%s' % (
                t, n['col'] + 1, tns, off, etns, eoff,
                'HoldHead' if n['kind'] in ('hold', 'roll') else 'Tap')))
            if n['kind'] in ('hold', 'roll'):
                if tns == 'Miss':
                    ht, hns = to_engine(row_time) + 0.2, 'Missed'
                elif info[2]:
                    drop = to_engine(info[0]) + info[1] * rate
                    ht, hns = drop + HOLD_WINDOW * rate, 'LetGo'
                    truth['letgo'] += 1
                else:
                    ht, hns = to_engine(n['end']), 'Held'
                events.append((ht, 'H\t%.6f\t%d\t%s' % (ht, n['col'] + 1, hns)))

    for n in notes:
        if n['kind'] == 'mine' and rnd.random() < 0.2:
            press(n['col'], n['time'] + rnd.uniform(-0.03, 0.03) * rate)
            t = to_engine(n['time'])
            events.append((t, 'M\t%.6f\t%d\tHitMine' % (t, n['col'] + 1)))
            truth['mine'] += 1

    # Stray taps between notes.
    times = [n['time'] for n in notes]
    strays = 0
    while strays < 25:
        i = rnd.randrange(1, len(times))
        a, b = times[i - 1], times[i]
        if b - a < 0.2:
            continue
        col = rnd.randrange(4)
        mid = (a + b) / 2
        # Not on top of a note, hold or mine in that column.
        if any(n['col'] == col and n['time'] - 0.2 < mid < n['end'] + 0.2
               for n in notes):
            continue
        press(col, mid)
        strays += 1
    truth['stray'] = strays

    events.sort(key=lambda e: e[0])
    end = max(e[0] for e in events) + 1.0
    header = [
        ('ArrowReplay', 1), ('Player', 'P1'), ('Profile', 'TestPlayer'),
        ('SongDir', args.virtual_path.rsplit('/', 1)[0] + '/'),
        ('SimfilePath', args.virtual_path),
        ('Title', tags.get('TITLE', '')), ('Subtitle', ''),
        ('Artist', tags.get('ARTIST', '')),
        ('StepsType', 'StepsType_Dance_Single'),
        ('Difficulty', 'Difficulty_' + chart[2]), ('Meter', chart[3]),
        ('Description', chart[1]), ('ChartName', ''), ('ChartHash', ''),
        ('Columns', 4), ('Rate', '%.6f' % rate),
        ('GlobalOffset', '%.6f' % g), ('GameMode', 'ITG'),
        ('Windows', ' '.join(['W%d=%.6f' % (i + 1, w + ADD) for i, w in enumerate(WINDOWS)] +
                             ['W0=%.6f' % W0, 'Hold=%.6f' % HOLD_WINDOW,
                              'Mine=%.6f' % MINE_WINDOW, 'Roll=0.351500'])),
        ('Date', '2026-10-22 20:00:00'), ('Autoplay', 0),
    ]
    with open(args.out, 'w', encoding='utf-8', newline='\n') as f:
        for k, v in header:
            f.write('%s\t%s\n' % (k, v))
        f.write('---\n')
        for _, line in events:
            f.write(line + '\n')
        f.write('END\t%.6f\t0\tScreenEvaluationStage\n' % end)
    print(truth)
    if args.truth:
        with open(args.truth, 'w', encoding='utf-8') as f:
            json.dump(truth, f, indent=1)


if __name__ == '__main__':
    main()
