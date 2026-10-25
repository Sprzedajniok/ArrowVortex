"""Checks the output of ArrowVortex --video-sync against make_test_recording.py.

usage: check_video_sync.py OUT.txt TRUTH.json
       check_video_sync.py OUT.txt --not-found

The offset has to be within a few milliseconds of where the play is in the
recording, and the play has to be found with all parts of the song. With
--not-found, the output has to be an error instead.
"""
import json
import sys

# The envelopes have a hop of 5 ms, plus resampling.
TOLERANCE = 0.006


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    try:
        lines = open(sys.argv[1], encoding='utf-8').read().splitlines()
    except OSError as e:
        print('FAIL: no output from ArrowVortex: %s' % e)
        return 1
    out = dict(line.split('\t', 1) for line in lines if '\t' in line)
    print(out)
    if sys.argv[2] == '--not-found':
        if 'Error' in out and 'Offset' not in out:
            print('OK: not found (%s)' % out['Error'])
            return 0
        print('FAIL: the play was found in a recording without it')
        return 1
    truth = json.load(open(sys.argv[2], encoding='utf-8'))
    if 'Offset' not in out:
        print('FAIL: %s' % out.get('Error', 'no offset'))
        return 1
    error = float(out['Offset']) - truth['offset']
    ok = abs(error) <= TOLERANCE and out['Votes'] == out['Parts']
    print('%s: offset %s, %+.1f ms from %.4f, %s of %s parts' % (
        'OK' if ok else 'FAIL', out['Offset'], error * 1000, truth['offset'],
        out['Votes'], out['Parts']))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
