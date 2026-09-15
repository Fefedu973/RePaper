#!/usr/bin/env python3
"""Compile the actual upstream/patched FBController and verify offline input mapping."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time
import xml.etree.ElementTree as ET

HERE = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--appload-source', required=True, type=Path)
parser.add_argument('--output', required=True, type=Path)
args = parser.parse_args()
source = args.appload_source.resolve()
output = args.output.resolve()
output.mkdir(parents=True, exist_ok=False)
patch_path = HERE.parent / 'windowed-input.patch'
records = []

def command(argv, log, **kwargs):
    started = time.monotonic()
    result = subprocess.run([str(value) for value in argv], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=120, **kwargs)
    log.write_bytes(result.stdout)
    records.append({'command': [str(value) for value in argv], 'exit_code': result.returncode,
                    'elapsed_seconds': round(time.monotonic() - started, 3), 'log': str(log.relative_to(output))})
    return result

for variant in ['before', 'after']:
    destination = output / variant
    shutil.copytree(source / 'src/qtfb', destination / 'src/qtfb')
    # Windows checkouts can contain CRLF. Normalize only the private build copies,
    # identically to the module build, before feeding GNU patch its LF diff.
    for name in ['FBController.cpp', 'FBController.h']:
        path = destination / 'src/qtfb' / name
        path.write_text(path.read_text())

with patch_path.open('rb') as patch:
    applied = command(['patch', '--batch', '--forward', '--ignore-whitespace', '-d', output / 'after', '-p1'],
                      output / 'patch.log', stdin=patch)
    if applied.returncode:
        raise SystemExit('Patch application failed; see patch.log')

# Qt 6.2 lacks QQuickItem::setFocusPolicy. This constructor-only compatibility
# adaptation is identical in both private test copies, with no mapping/render edits.
qt_version = subprocess.check_output(['qmake6', '-query', 'QT_VERSION'], text=True).strip()
compatibility = []
if tuple(int(part) for part in qt_version.split('.')[:2]) < (6, 7):
    for variant in ['before', 'after']:
        header = output / variant / 'src/qtfb/FBController.h'
        data = header.read_text()
        assert data.count('setFocusPolicy(Qt::StrongFocus);') == 1
        header.write_text(data.replace('setFocusPolicy(Qt::StrongFocus);', ''))
    compatibility.append('Removed constructor setFocusPolicy(Qt::StrongFocus) in both private copies for host Qt ' + qt_version)

def build(variant):
    directory = output / ('build-' + variant)
    configured = command(['cmake', '-S', HERE, '-B', directory,
                          '-DCMAKE_BUILD_TYPE=Release', '-DFBCONTROLLER_SOURCE=' + str(output / variant)],
                         output / ('configure-' + variant + '.log'))
    if configured.returncode:
        raise RuntimeError('Configure failed: ' + variant)
    built = command(['cmake', '--build', directory, '--parallel', '2'], output / ('build-' + variant + '.log'))
    if built.returncode:
        raise RuntimeError('Build failed: ' + variant)
    return directory / 'fbcontroller-input-tests'

with ThreadPoolExecutor(max_workers=2) as executor:
    executables = dict(zip(['before', 'after'], executor.map(build, ['before', 'after'])))

env = {'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8', 'QT_QPA_PLATFORM': 'offscreen',
       'QT_QUICK_BACKEND': 'software', 'QSG_RENDER_LOOP': 'basic'}
for key in ['HOME', 'XDG_CONFIG_HOME', 'XDG_DATA_HOME', 'XDG_CACHE_HOME', 'XDG_RUNTIME_DIR']:
    path = output / 'profile' / key.lower()
    path.mkdir(parents=True, mode=0o700)
    env[key] = str(path)

before = command([executables['before'], 'scaledCenter', 'letterboxCenter', 'renderSignatures',
                  '-o', str(output / 'before.xml') + ',xml', '-o', str(output / 'before.txt') + ',txt'],
                 output / 'before-console.log', env=env)
after = command([executables['after'], '-o', str(output / 'after.xml') + ',xml',
                 '-o', str(output / 'after.txt') + ',txt'], output / 'after-console.log', env=env)

def incidents(path):
    result = []
    for function in ET.parse(path).getroot().findall('TestFunction'):
        for incident in function.findall('Incident'):
            result.append({'test': function.get('name'), 'type': incident.get('type'),
                           'data': incident.findtext('DataTag') or '',
                           'description': incident.findtext('Description') or ''})
    return result

before_checks = incidents(output / 'before.xml')
after_checks = incidents(output / 'after.xml')
before_failed = [row for row in before_checks if row['type'] == 'fail']
after_failed = [row for row in after_checks if row['type'] == 'fail']
signatures = {variant: re.findall(r'RENDER_SHA256=([0-9a-f]{64})', (output / (variant + '.txt')).read_text())
              for variant in ['before', 'after']}
render_unchanged = len(signatures['before']) == 1 and signatures['before'] == signatures['after']
passed = (before.returncode != 0 and {row['test'] for row in before_failed} == {'scaledCenter', 'letterboxCenter'}
          and after.returncode == 0 and not after_failed and render_unchanged)
report = {
    'status': 'PASS' if passed else 'FAIL', 'host_qt_version': qt_version,
    'source': str(source), 'patch_sha256': hashlib.sha256(patch_path.read_bytes()).hexdigest(),
    'upstream_fbcontroller_sha256': hashlib.sha256((source / 'src/qtfb/FBController.cpp').read_bytes()).hexdigest(),
    'patched_fbcontroller_sha256': hashlib.sha256((output / 'after/src/qtfb/FBController.cpp').read_bytes()).hexdigest(),
    'compatibility_adaptations': compatibility,
    'baseline_exit_code': before.returncode, 'baseline_expected_failures': before_failed,
    'patched_exit_code': after.returncode, 'patched_passes': sum(row['type'] == 'pass' for row in after_checks),
    'patched_failures': after_failed, 'paint_output_unchanged': render_unchanged,
    'paint_sha256': signatures, 'checks': after_checks, 'commands': records,
    'scope': ['Real compiled FBController paint and inverse mapping',
              '1620x2160 framebuffer at fullscreen and resized window dimensions',
              'Four rotations, aspect-fit margins, stretch, crop, pad and disabled scaling',
              'Mouse events and Qt-delivered touch events through to captured QTFB packets',
              'Null images, zero dimensions, invalid coordinates and rejected letterbox touch presses'],
    'limitations': ['Host Qt runtime; no tablet or proprietary QML is used.',
                    'Pre-existing off-image gesture lifecycle is unchanged: touch updates can fall back to (0,0), and off-image mouse releases can be dropped.',
                    'Rotation tests establish painting and input mapping, not rotated partial-refresh regions.',
                    'QTFB transport is a packet collector; physical input devices and live AppLoad integration are not exercised.']
}
(output / 'validation.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps({key: report[key] for key in ['status', 'host_qt_version', 'baseline_exit_code', 'baseline_expected_failures', 'patched_exit_code', 'patched_passes', 'patched_failures', 'paint_output_unchanged', 'patch_sha256']}, indent=2))
raise SystemExit(0 if passed else 1)
