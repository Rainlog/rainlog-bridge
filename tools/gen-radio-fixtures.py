#!/usr/bin/env python3
"""Extract OOK durations from Tim's documented TX5U CU8 captures, 250 ksample/s.

Pass the directory containing count14/count15/count25/tips18to21 captures.
Only pulse durations are committed, not the original multi-megabyte IQ files.
"""
from pathlib import Path
import sys

root = Path(sys.argv[1])
out = Path(__file__).resolve().parents[1] / 'tests/fixtures/tx5u-pulses.txt'
lines = ['# TX5U pulse durations from recorded CU8 captures, 250 ksample/s.']
for name in ['count14', 'count15', 'count25', 'tips18to21']:
    data = (root / (name + '_433.92M_250k.cu8')).read_bytes()
    powers = [(data[i]-128)**2 + (data[i+1]-128)**2 for i in range(0, len(data), 2)]
    # The documented captures have two well-separated carrier/noise levels.
    # Use the midpoint between the 10th and 90th percentile power levels.
    ordered = sorted(powers)
    threshold = (ordered[len(ordered)//10] + ordered[len(ordered)*9//10]) / 2
    previous = powers[0] > threshold
    start = 0
    lines.append('# ' + name)
    durations = []
    for i, power in enumerate(powers[1:], 1):
        level = power > threshold
        if level != previous:
            durations.append([int(previous), (i-start)*4])
            previous, start = level, i
    durations.append([int(previous), (len(powers)-start)*4])
    # Match the receiver's 80 us software filter: merge short glitches with
    # their equal-level neighbors, retaining the total elapsed time.
    i = 1
    while i + 1 < len(durations):
        if durations[i][1] < 80 and durations[i-1][0] == durations[i+1][0]:
            durations[i-1][1] += durations[i][1] + durations[i+1][1]
            del durations[i:i+2]
            i = max(1, i-1)
        else:
            i += 1
    lines.extend(f'{level} {duration}' for level, duration in durations)
out.write_text('\n'.join(lines) + '\n')
print(out)
