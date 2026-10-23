"""Compares an ArrowVortex replay report with the judgment counts that
make_test_replay.py generated.

usage: check_report.py REPORT.txt TRUTH.json

The report comes from "ArrowVortex --replay-report REPLAY SIMFILE REPORT.txt".
"""
import json
import sys


def main():
    report = {}
    with open(sys.argv[1], encoding='utf-8') as f:
        for line in f:
            key, _, value = line.rstrip('\n').partition('\t')
            report[key] = value
    with open(sys.argv[2], encoding='utf-8') as f:
        truth = json.load(f)
    print(''.join('%s\t%s\n' % kv for kv in report.items()))
    if 'Error' in report:
        sys.exit('ArrowVortex could not show the replay: ' + report['Error'])

    count = lambda key: int(report[key])
    checks = [
        ('Fantastic', count('FantasticPlus') + count('Fantastic'), truth['W1']),
        ('Excellent', count('Excellent'), truth['W2']),
        ('Great', count('Great'), truth['W3']),
        ('Decent', count('Decent'), truth['W4']),
        ('Way Off', count('WayOff'), truth['W5']),
        ('Miss', count('Miss'), truth['Miss']),
        ('Holds let go', count('HoldsLetGo'), truth['letgo']),
        ('Mines hit', count('MinesHit'), truth['mine']),
        ('Stray taps', count('StrayTaps'), truth['stray']),
        ('Early hits fixed', count('EarlyHitsFixed'), truth['rescored']),
        ('Hits without a press', count('Hits') - count('HitsMatched'), 0),
        ('Notes not judged', count('NotJudged'), 0),
    ]
    failed = False
    for name, got, expected in checks:
        ok = got == expected
        failed |= not ok
        print('%-22s %5d  expected %5d  %s' % (name, got, expected, 'ok' if ok else 'WRONG'))
    if report.get('Warning'):
        print('unexpected warning: ' + report['Warning'])
        failed = True
    if failed:
        sys.exit('the replay report does not match the generated replay')
    print('replay report matches')


if __name__ == '__main__':
    main()
