"""Compile the actual AIORS daemon CLI/GPIO code with its supported GPIO stub.

The current SvxLink LED worker talks to the real companion CLI thread using
temporary command/reply files. No GPIO/I2C/UART hardware is controlled.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bsp', type=Path, required=True)
    args = parser.parse_args()
    bsp = args.bsp.resolve()
    repo = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix='aiors-bsp-virtual-') as directory:
        output = Path(directory)
        flags = ['-O2', '-Wall', '-Wextra', '-Werror', '-DGPIO_FORCE_STUB',
                 '-DCMD_DIR="/tmp/svxlink-aiors-bridge-test"', '-I'+str(bsp/'include'), '-I'+str(bsp/'src')]
        names = 'cfg cli gpio i2c uart usb cm108 radio_type radio_sa818 radio_sa858 radio_ext util log routing'.split()
        def compile_source(name):
            subprocess.run(['gcc', '-std=gnu11', *flags, '-c', str(bsp/'src'/f'{name}.c'),
                            '-o', str(output/f'{name}.o')], check=True)
        with ThreadPoolExecutor(max_workers=4) as pool:
            list(pool.map(compile_source, names))
        binary = output/'bsp-bridge'
        subprocess.run(['g++', '-std=c++11', '-pthread', *flags,
                        '-I'+str(repo/'src/svxlink/svxlink'), str(repo/'tests/aiors_bsp_bridge.cpp'),
                        str(repo/'src/svxlink/svxlink/AiorsLedClient.cpp'),
                        *map(str, sorted(output.glob('*.o'))), '-lm', '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == '__main__':
    main()
