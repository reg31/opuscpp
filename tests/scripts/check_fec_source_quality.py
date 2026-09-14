"""Compare source-referenced FEC quality from two fec_source_quality binaries."""
import argparse
import json
import math
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('current', type=Path)
    parser.add_argument('official', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    cases = [(1, 10, 24000, 1), (1, 20, 16000, 1), (1, 20, 24000, 1),
             (1, 20, 32000, 1), (2, 20, 32000, 0), (2, 20, 48000, 1)]
    fields = ('fidelity_fec next_fec transition_fec fidelity_ref next_ref transition_ref '
              'fidelity_plc self_consistency step_fec step_ref').split()
    rows, counts = [], [0, 0, 0, 0]
    for profile in range(3):
        for channels, ms, bitrate, vbr in cases:
            key = [channels, ms, bitrate, profile, vbr]
            row = {'key': key}
            for side in ('current', 'official'):
                command = [str(getattr(args, side).resolve()), *map(str, key)]
                result = subprocess.run(command, capture_output=True, text=True, timeout=120)
                values = {k: float(v) for k, v in re.findall(r'(\w+)=([-+0-9.eE]+)', result.stdout)}
                row[side] = dict(command=command, exit=result.returncode, stdout=result.stdout, stderr=result.stderr,
                                 fields={f: values.get(f) for f in fields})
                if result.returncode or not all(f in values and math.isfinite(values[f]) for f in fields):
                    rows.append(row)
                    args.output.write_text(json.dumps({'rows': rows, 'error': 'incomplete measurement'}, indent=2))
                    raise SystemExit(f'{side}: failed measurement {key}')
            current, official = row['current']['fields'], row['official']['fields']
            passed = [current[f] <= official[f] for f in fields[:3]]
            passed.append(current['fidelity_fec'] < current['fidelity_plc'])
            row['criteria_pass'] = passed
            counts = [a + b for a, b in zip(counts, passed)]
            rows.append(row)
    args.output.write_text(json.dumps({'counts': counts, 'rows': rows}, indent=2))
    print(f'FEC source criteria C1/C2/C3/C4: {counts} out of 18')
    return 0 if counts == [18, 18, 18, 18] else 1


if __name__ == '__main__':
    raise SystemExit(main())
