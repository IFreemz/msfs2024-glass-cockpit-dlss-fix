# GPL-3.0-only with the additional terms in NOTICE.md.
"""Offline v1 rendering regressions. Runs only the local fake-simulator fixture."""
from pathlib import Path
import math
import re
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
fixture = Path(sys.argv[1]).resolve() if len(sys.argv)>1 else root / 'build/test/FlightSimulator2024.exe'
results = {}
for mode in ('taa-ls0', 'gauss-ls35', 'still-occluder', 'still-dim-atlas', 'still-swap', 'still-blank', 'still-windows'):
    run = subprocess.run([str(fixture), '--motion', '--only', mode], cwd=fixture.parent,
                         capture_output=True, text=True, timeout=120)
    print(run.stdout, end='', flush=True)
    assert run.returncode == 0, (mode, run.returncode, run.stderr)
    rows = re.findall(r'^(\S+)\s+error=([^\r\n]+)', run.stdout, re.M)
    assert rows, ('No rendering measurements', mode)
    for name, measurements in rows:
        values = dict((key, float(value)) for key, value in
                      re.findall(r'(\w+)=([\d.eE+-]+)', 'error=' + measurements))
        assert all(math.isfinite(v) for v in values.values()), (name, values)
        # Normalized interior error and stale-content error after changing the
        # display must stay low. Compare occlusion against its negative control.
        if name == 'still-windows':
            # This negative-control picture is deliberately magenta in the
            # windows left to the upscaler. Judge the surrounding screen only.
            assert values['screen_dropped'] < 0.01, (name, values)
        else:
            assert values['error'] < 0.05 and values['after_change'] < 0.05, (name, values)
        results[name] = values
off = results['still-occluder-check-off']['occluder_error']
on = results['still-occluder']['occluder_error']
assert off > 0.05 and on < 0.01 and on < off / 10, (off, on)
assert results['still-swap']['swap_error'] < 0.05
assert results['taa-ls0']['flicker'] < 0.05
print(f'PASS {len(results)} motion/occlusion/brightness/history/texture-replacement cases')
