#!/usr/bin/env python3
"""Verify ARM FBController composition against the SDK's real e-paper painter node."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import re
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
args = parser.parse_args()
source, output = args.appload_source.resolve(), args.output.resolve()
output.mkdir(parents=True, exist_ok=False)
sdk_root = args.sdk_root.resolve()
sdk = sdk_root / 'sysroots/cortexa53-crypto-remarkable-linux'
epaper = sdk / 'usr/lib/plugins/scenegraph/libqsgepaper.so'
epaper_sha = hashlib.sha256(epaper.read_bytes()).hexdigest()
if epaper_sha != '3f76b7db328f7e16cde2d360ebe4a1db043a113cd2386d4c995cc4d25a0eeaec':
    raise SystemExit('The diagnostic e-paper node ABI must be reviewed for this SDK library hash.')
records = []


def command(argv, name, **kwargs):
    argv = [str(value) for value in argv]
    started = time.monotonic()
    result = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=180, **kwargs)
    (output / (name + '.log')).write_bytes(result.stdout)
    records.append({'command': argv, 'exit_code': result.returncode,
                    'elapsed_seconds': round(time.monotonic() - started, 3), 'log': name + '.log'})
    return result


def patch(name, destination):
    with (HERE.parent / name).open('rb') as handle:
        result = command(['patch', '--batch', '--forward', '-p1', '-d', destination],
                         name + '-' + destination.name, stdin=handle)
    if result.returncode:
        raise RuntimeError('Patch failed: ' + name)


shutil.copytree(source / 'src/qtfb', output / 'before/src/qtfb')
for path in (output / 'before').rglob('*'):
    if path.suffix in ['.cpp', '.h']:
        path.write_text(path.read_text())
patch('windowed-input.patch', output / 'before')
shutil.copytree(output / 'before', output / 'after')
patch('window-rendering.patch', output / 'after')

sdk_result = subprocess.run(['/bin/bash', '-c', 'source "$1" >/dev/null; env -0', 'sdk',
    str(sdk_root / 'environment-setup-cortexa53-crypto-remarkable-linux')],
    env={'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8'}, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
build_env = dict(entry.decode().split('=', 1) for entry in sdk_result.stdout.split(b'\0') if entry)


def build(variant):
    binary = output / ('build-' + variant)
    configured = command(['cmake', '-S', HERE, '-B', binary, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
        '-DFBCONTROLLER_SOURCE=' + str(output / variant)], 'configure-' + variant, env=build_env)
    if configured.returncode or command(['cmake', '--build', binary, '--parallel', '2'],
                                       'build-' + variant, env=build_env).returncode:
        raise RuntimeError('Build failed: ' + variant)


with ThreadPoolExecutor(max_workers=2) as executor:
    list(executor.map(build, ['before', 'after']))

runtime = args.stage.resolve() / 'runtime'
private = output / 'test-runtime'
(private / 'platforms').mkdir(parents=True)
(private / 'lib').mkdir()
shutil.copy2(sdk / 'usr/lib/plugins/platforms/libqoffscreen.so', private / 'platforms')
shutil.copy2(sdk / 'usr/lib/libQt6Test.so.6.10.3', private / 'lib/libQt6Test.so.6')
shutil.copy2(sdk / 'usr/lib/libdrm.so.2.4.0', private / 'lib/libdrm.so.2')
env = {'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8', 'QT_QPA_PLATFORM': 'offscreen',
       'QT_QUICK_BACKEND': 'software', 'QSG_RENDER_LOOP': 'basic',
       'QT_PLUGIN_PATH': str(runtime / 'plugins'), 'QT_QPA_PLATFORM_PLUGIN_PATH': str(private / 'platforms'),
       'QT_QPA_FONTDIR': str(runtime / 'fonts'), 'FONTCONFIG_FILE': str(runtime / 'fonts/fonts.conf'),
       'REPAPER_EPAPER_PLUGIN': str(epaper)}
qemu = sdk_root / 'sysroots/x86_64-codexsdk-linux/usr/bin/qemu-aarch64'
results = {}
for variant in ['before', 'after']:
    child_env = env.copy()
    images = output / (variant + '-images')
    images.mkdir()
    child_env['REPAPER_RENDER_OUTPUT'] = str(images)
    for key in ['HOME', 'XDG_CONFIG_HOME', 'XDG_DATA_HOME', 'XDG_CACHE_HOME', 'XDG_RUNTIME_DIR']:
        path = output / ('profile-' + variant) / key.lower()
        path.mkdir(mode=0o700, parents=True)
        child_env[key] = str(path)
    base = ['/usr/bin/unshare', '-n', qemu, '-L', sdk, '-E',
            'LD_LIBRARY_PATH=' + str(runtime / 'lib') + ':' + str(private / 'lib')]
    results[variant] = command([*base, output / ('build-' + variant) / 'window-rendering-tests',
        '-o', str(output / (variant + '.txt')) + ',txt', '-o', str(output / (variant + '.xml')) + ',xml'],
        'run-' + variant, env=child_env)
    selection = ['renderSignatures'] if variant == 'before' else []
    results[variant + '-mapping'] = command([*base, output / ('build-' + variant) / 'window-input-regressions',
        *selection, '-o', str(output / (variant + '-mapping.txt')) + ',txt',
        '-o', str(output / (variant + '-mapping.xml')) + ',xml'], 'run-' + variant + '-mapping', env=child_env)


def incidents(name):
    return [{'test': function.get('name'), 'type': incident.get('type'), 'data': incident.findtext('DataTag') or '',
             'description': incident.findtext('Description') or ''}
            for function in ET.parse(output / (name + '.xml')).getroot().findall('TestFunction')
            for incident in function.findall('Incident')]


before, after, mapping = incidents('before'), incidents('after'), incidents('after-mapping')
baseline_failures = [item for item in before if item['type'] == 'fail']
failures = [item for item in after + mapping if item['type'] == 'fail']
signatures = {variant: re.findall(r'RENDER_SHA256=([0-9a-f]{64})',
    (output / (variant + '-mapping.txt')).read_text()) for variant in ['before', 'after']}
unchanged = len(signatures['before']) == 1 and signatures['before'] == signatures['after']
passed = (results['before'].returncode != 0
    and len(baseline_failures) == 66
    and {item['test'] for item in baseline_failures} == {
        'composedScene', 'movedWindowAndClippedRefresh', 'restoresCallerPainterState'}
    and results['after'].returncode == 0 and results['after-mapping'].returncode == 0
    and not failures and unchanged)
report = {'status': 'PASS' if passed else 'FAIL', 'qt_version': '6.10.3', 'architecture': 'aarch64',
    'source': str(source), 'sdk_root': str(sdk_root), 'stage': str(args.stage.resolve()),
    'patch_sha256': hashlib.sha256((HERE.parent / 'window-rendering.patch').read_bytes()).hexdigest(),
    'epaper_plugin_sha256': epaper_sha, 'baseline_expected_failures': baseline_failures,
    'rendering_passes': sum(item['type'] == 'pass' for item in after),
    'input_regression_passes': sum(item['type'] == 'pass' for item in mapping),
    'patched_failures': failures, 'identity_paint_unchanged': unchanged, 'identity_paint_sha256': signatures,
    'checks': after, 'input_checks': mapping, 'commands': records,
    'scope': ['Actual ARM FBController and actual SDK libqsgepaper painter node',
              'Incoming scene transforms passed intact by the native e-paper node',
              'Framebuffer and scene rotations, 1x/2x scale, window movement and clipped repaint',
              'Painter state restoration and 85 existing input/rendering regression checks'],
    'limitations': ['The e-paper node is invoked directly without its renderer, DRM screen or real tablet.',
                    'No native Xochitl process or physical e-paper refresh is exercised.',
                    'The SDK diagnostic private ABI is pinned to the recorded plugin SHA256.']}
(output / 'validation.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps({key: report[key] for key in ['status', 'patch_sha256', 'epaper_plugin_sha256',
    'rendering_passes', 'input_regression_passes', 'patched_failures', 'identity_paint_unchanged']}, indent=2))
raise SystemExit(0 if passed else 1)
