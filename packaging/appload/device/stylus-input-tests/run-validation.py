#!/usr/bin/env python3
"""Build real FBController/QML with the ARM SDK and test native Qt tablet delivery offline."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time
import xml.etree.ElementTree as ET

HERE = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--appload-source', type=Path, required=True)
parser.add_argument('--stage', type=Path, required=True)
parser.add_argument('--sdk-root', type=Path, default=Path('/opt/repaper-sdk/5.8.203'))
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--native-mouse-fix', action='store_true',
                    help='Apply the follow-up native Xochitl mouse/stylus correction')
args = parser.parse_args()
source, output = args.appload_source.resolve(), args.output.resolve()
output.mkdir(parents=True, exist_ok=False)
records = []

def command(argv, name, **kwargs):
    argv = [str(value) for value in argv]
    started = time.monotonic()
    result = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=180, **kwargs)
    (output / (name + '.log')).write_bytes(result.stdout)
    records.append({'command': argv, 'exit_code': result.returncode,
                    'elapsed_seconds': round(time.monotonic() - started, 3), 'log': name + '.log'})
    return result

def patch(name, target):
    with (HERE.parent / name).open('rb') as handle:
        result = command(['patch', '--batch', '--forward', '-p1', '-d', target], name + '-' + target.name, stdin=handle)
    if result.returncode:
        raise RuntimeError('Patch failed: ' + name)

for folder in ['src/qtfb', 'shim', 'resources']:
    shutil.copytree(source / folder, output / 'before' / folder)
for path in (output / 'before').rglob('*'):
    if path.suffix in ['.cpp', '.h', '.qml', '.pro', '.qrc']:
        path.write_text(path.read_text())
patch('windowed-input.patch', output / 'before')
shutil.copytree(output / 'before', output / 'after')
patch('stylus-input.patch', output / 'after')
if args.native_mouse_fix:
    patch('native-stylus-mouse.patch', output / 'after')
patch('shim-private-input.patch', output / 'after')

sdk_root = args.sdk_root.resolve()
sdk = sdk_root / 'sysroots/cortexa53-crypto-remarkable-linux'
qemu = sdk_root / 'sysroots/x86_64-codexsdk-linux/usr/bin/qemu-aarch64'
sdk_result = subprocess.run(['/bin/bash', '-c', 'source "$1" >/dev/null; env -0', 'sdk',
                             str(sdk_root / 'environment-setup-cortexa53-crypto-remarkable-linux')],
                            env={'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8'},
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
build_env = dict(entry.decode().split('=', 1) for entry in sdk_result.stdout.split(b'\0') if entry)

def build(variant):
    binary = output / ('build-' + variant)
    options = ['-DSTYLUS_BASELINE=ON'] if variant == 'before' else []
    configured = command(['cmake', '-S', HERE, '-B', binary, '-G', 'Ninja',
                          '-DCMAKE_BUILD_TYPE=Release', '-DFBCONTROLLER_SOURCE=' + str(output / variant), *options],
                         'configure-' + variant, env=build_env)
    if configured.returncode:
        raise RuntimeError('Configure failed: ' + variant)
    result = command(['cmake', '--build', binary, '--parallel', '2'], 'build-' + variant, env=build_env)
    if result.returncode:
        raise RuntimeError('Build failed: ' + variant)

with ThreadPoolExecutor(max_workers=2) as executor:
    list(executor.map(build, ['before', 'after']))
configured = command(['cmake', '-S', output / 'after/shim', '-B', output / 'build-shim', '-G', 'Ninja',
                      '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_CXX_STANDARD=17'], 'configure-shim', env=build_env)
if configured.returncode or command(['cmake', '--build', output / 'build-shim', '--target', 'qtfb-shim', '--parallel', '2'],
                                    'build-shim', env=build_env).returncode:
    raise RuntimeError('Shim build failed')

runtime = args.stage.resolve() / 'runtime'
private = output / 'test-runtime'
(private / 'platforms').mkdir(parents=True)
(private / 'lib').mkdir()
shutil.copy2(sdk / 'usr/lib/plugins/platforms/libqoffscreen.so', private / 'platforms')
shutil.copy2(sdk / 'usr/lib/libQt6Test.so.6.10.3', private / 'lib/libQt6Test.so.6')
env = {'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8', 'QT_QPA_PLATFORM': 'offscreen',
       'QT_QUICK_BACKEND': 'software', 'QSG_RENDER_LOOP': 'basic', 'QT_PLUGIN_PATH': str(runtime / 'plugins'),
       'QT_QPA_PLATFORM_PLUGIN_PATH': str(private / 'platforms'), 'QML_IMPORT_PATH': str(runtime / 'qml'),
       'QML2_IMPORT_PATH': str(runtime / 'qml'), 'QT_QPA_FONTDIR': str(runtime / 'fonts'),
       'FONTCONFIG_FILE': str(runtime / 'fonts/fonts.conf')}
results = {}
for variant in ['before', 'after']:
    child_env = env.copy()
    if variant == 'after':
        child_env['REPAPER_STYLUS_PACKET_OUTPUT'] = str(output / 'fbcontroller-packets.json')
        child_env['REPAPER_NATIVE_MOUSE_PACKET_OUTPUT'] = str(output / 'native-mouse-packets.json')
        child_env['REPAPER_NATIVE_MOUSE_SCROLL_PACKET_OUTPUT'] = str(output / 'native-mouse-scroll-packets.json')
    for key in ['HOME', 'XDG_CONFIG_HOME', 'XDG_DATA_HOME', 'XDG_CACHE_HOME', 'XDG_RUNTIME_DIR']:
        path = output / ('profile-' + variant) / key.lower()
        path.mkdir(mode=0o700, parents=True)
        child_env[key] = str(path)
    results[variant] = command(['/usr/bin/unshare', '-n', qemu, '-L', sdk,
        '-E', 'LD_LIBRARY_PATH=' + str(runtime / 'lib') + ':' + str(private / 'lib'),
        output / ('build-' + variant) / 'stylus-input-tests',
        '-o', str(output / (variant + '.txt')) + ',txt', '-o', str(output / (variant + '.xml')) + ',xml'],
        'run-' + variant, env=child_env)

def incidents(variant):
    return [{'test': function.get('name'), 'type': incident.get('type'), 'data': incident.findtext('DataTag') or '',
             'description': incident.findtext('Description') or ''}
            for function in ET.parse(output / (variant + '.xml')).getroot().findall('TestFunction')
            for incident in function.findall('Incident')]

before, after = incidents('before'), incidents('after')
baseline_failures = [item for item in before if item['type'] == 'fail']
failures = [item for item in after if item['type'] == 'fail']
passed = (results['before'].returncode != 0 and len(baseline_failures) == 1
          and baseline_failures[0]['test'] == 'nativeTabletSequence'
          and results['after'].returncode == 0 and not failures)
report = {'status': 'PASS' if passed else 'FAIL', 'qt_version': '6.10.3', 'architecture': 'aarch64',
          'source': str(source), 'sdk_root': str(sdk_root), 'stage': str(args.stage.resolve()),
          'patch_sha256': hashlib.sha256((HERE.parent / 'stylus-input.patch').read_bytes()).hexdigest(),
          'native_mouse_patch_sha256': hashlib.sha256((HERE.parent / 'native-stylus-mouse.patch').read_bytes()).hexdigest()
                                      if args.native_mouse_fix else None,
          'shim_sha256': hashlib.sha256((output / 'build-shim/qtfb-shim.so').read_bytes()).hexdigest(),
          'baseline_expected_failures': baseline_failures, 'patched_passes': sum(item['type'] == 'pass' for item in after),
          'patched_failures': failures, 'checks': after, 'commands': records,
          'scope': ['Real ARM FBController with production StylusInputHandler.qml',
                    'QWindowSystemInterface native tablet events, without relying on synthetic mouse events',
                    'Pen and eraser pointer, pressure, movement, cancellation, window scaling/rotations and overlapping windows',
                    'Preserved physical mouse/touch delivery and no duplicate tablet mouse synthesis'],
          'limitations': ['QTFB transport is a packet collector in this suite.',
                          'Run run-linuxfb-validation.py for real shim/evdevtablet/button integration.',
                          'No tablet, proprietary Xochitl process or live network is accessed.']}
(output / 'validation.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps({key: report[key] for key in ['status', 'patch_sha256', 'patched_passes', 'baseline_expected_failures', 'patched_failures', 'shim_sha256']}, indent=2))
raise SystemExit(0 if passed else 1)
