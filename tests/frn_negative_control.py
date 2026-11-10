"""Restore only the old TX watchdog defect in an isolated sanitizer runtime."""
from pathlib import Path
import argparse
import json
import re
import shlex
import subprocess
import sys

repo = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', type=Path, required=True, help='Native sanitizer build')
parser.add_argument('--output', type=Path, required=True,
                    help='New directory for the isolated runtime, logs and result')
args = parser.parse_args()
sys.path.insert(0, str(repo/'tests'))
from soak_support import freeze_build

stage = args.output.resolve()
stage.mkdir(parents=True, exist_ok=False)
build = args.build.resolve()
runtime = freeze_build(build, stage)
directory = build/'svxlink/modules/frn/CMakeFiles/ModuleFrn.dir'
flags = (directory/'flags.make').read_text()
arguments = []
for name in ('CXX_DEFINES', 'CXX_INCLUDES', 'CXX_FLAGS'):
    arguments += shlex.split(re.search('^'+name+' = (.*)$', flags, re.M)[1])
source = (repo/'src/svxlink/modules/frn/QsoFrn.cpp').read_text()
needle = '  con_timeout_timer->reset();\n  frnTxBytes((uint64_t)nbytes);'
assert source.count(needle) == 1
source = source.replace(needle, '  // Negative control: restore receive-only watchdog.\n  frnTxBytes((uint64_t)nbytes);')
(stage/'QsoFrn.cpp').write_text(source)
subprocess.run(['g++', *arguments, '-I'+str(repo/'src/svxlink/modules/frn'),
                '-c', str(stage/'QsoFrn.cpp'), '-o', str(stage/'QsoFrn.o')], check=True)
subprocess.run(['g++', '-shared', '-fsanitize=address,undefined',
                str(directory/'ModuleFrn.cpp.o'), str(stage/'QsoFrn.o'), str(directory/'Utils.cpp.o'),
                '-lsigc-2.0', '-lgsm', '-o', str(runtime/'lib/ModuleFrn.so')], check=True)
completed = subprocess.run([sys.executable, str(repo/'tests/frn_routing_virtual.py'),
                '--build', str(runtime), '--frn-liveness', '--soak-log', str(stage/'svxlink.log')],
                capture_output=True, text=True, timeout=110)
output = completed.stdout + completed.stderr
(stage/'test.log').write_text(output)
assert completed.returncode != 0, 'The old watchdog defect unexpectedly passed'
assert 'Healthy long TX reconnected' in output, output[-6000:]
assert 'FRN connection watchdog timeout in state TX_AUDIO' in output, output[-6000:]
(stage/'result.json').write_text(json.dumps({
    'status':'expected_failure', 'restored_defect':'receive-only watchdog during healthy TX',
    'returncode':completed.returncode, 'detected_by':'65-second silent-peer transmission',
}, indent=2)+'\n')
print('PASS negative control: the old watchdog defect fails the new long-TX test for the expected reason.')
