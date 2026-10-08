"""Compile the actual recovery code with host fakes and run regression tests.

Usage: python tests/runtime/run.py [--compiler path/to/zig.exe]
Defaults to the CXX environment variable or c++ on PATH.
"""
import argparse
import os
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[2]
p = argparse.ArgumentParser()
p.add_argument('--compiler', default=os.environ.get('CXX', 'c++'))
args = p.parse_args()
out = root / 'build' / 'runtime-tests'
out.mkdir(parents=True, exist_ok=True)
exe = out / ('recovery.exe' if os.name == 'nt' else 'recovery')
command = [args.compiler]
if Path(args.compiler).stem == 'zig':
    command += ['c++']
command += ['-std=c++17', '-O0', '-g', '-Wall', '-Wextra',
            '-I' + str(root / 'tests/runtime/stubs'),
            '-I' + str(root / 'lib/MultiObserver/src'),
            str(root / 'tests/runtime/recovery.cpp'),
            str(root / 'lib/MultiObserver/src/MOMQTT.cpp'),
            str(root / 'lib/MultiObserver/src/MOWatchdog.cpp'), '-o', str(exe)]
subprocess.run(command, check=True)
subprocess.run([str(exe)], check=True)
