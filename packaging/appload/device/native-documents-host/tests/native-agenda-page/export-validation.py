#!/usr/bin/env python3
"""Collect completed offline test evidence; never connects to a device."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import shutil
import xml.etree.ElementTree as ET

parser = argparse.ArgumentParser()
parser.add_argument('--host-build', required=True, type=Path)
parser.add_argument('--arm-build', required=True, type=Path)
parser.add_argument('--font', required=True, type=Path)
parser.add_argument('--host-preview', required=True, type=Path)
parser.add_argument('--arm-preview', required=True, type=Path)
parser.add_argument('--output', required=True, type=Path)
args = parser.parse_args()
repo = Path(__file__).resolve().parents[6]
args.output.mkdir(parents=True, exist_ok=True)
report = {'status': 'PASS', 'scope': 'offline Qt host and ARM64 QEMU; no tablet execution', 'suites': {}, 'sources': {}}
for architecture, source in [('host', args.host_build), ('arm64', args.arm_build)]:
    xml = ET.parse(source/'test-results.xml').getroot()
    incidents = xml.findall('.//Incident')
    passed = sum(item.get('type') == 'pass' for item in incidents)
    failed = [item.get('type') for item in incidents if item.get('type') != 'pass']
    if failed or passed < 18:
        raise SystemExit(f'{architecture} tests incomplete or failed: {passed}, {failed}')
    destination = args.output/architecture
    destination.mkdir(exist_ok=True)
    for name in ['configure.log', 'build.log', 'test-results.txt', 'test-results.xml']:
        shutil.copy2(source/name, destination/name)
    text = (source/'test-results.txt').read_text()
    timing = re.search(r'longTitle512Milliseconds (\d+)', text)
    report['suites'][architecture] = {'passed': passed, 'failed': 0,
        'qt': xml.findtext('.//QtVersion'),
        'long_title_512_ms_informative': int(timing.group(1)) if timing else None,
        'executable_sha256': hashlib.sha256((source/'native-agenda-page-tests').read_bytes()).hexdigest()}
for path in ([repo/'packaging/appload/device/native-documents-host'/name for name in
             ['NativeAgendaPage.cpp','NativeAgendaPage.h','NativeAgendaLayout.cpp','NativeAgendaLayout.h']]
    + [repo/'tools/build-appload-module.sh', repo/'packaging/appload/device/native-documents-host.patch']):
    report['sources'][str(path.relative_to(repo))] = hashlib.sha256(path.read_bytes()).hexdigest()
for name in ['NativeObjectAccess.cpp','NativeObjectAccess.h','NativeObjectSnapshot.h']:
    path = repo/'extensions/editor-common'/name
    report['sources'][str(path.relative_to(repo))] = hashlib.sha256(path.read_bytes()).hexdigest()
report['native_font_sha256_private_cache_only'] = hashlib.sha256(args.font.read_bytes()).hexdigest()
report['native_font_packaged'] = False
report['template'] = 'LS Dayplanner'
report['native_bounds_from_controller'] = True
report['maximum_native_strokes'] = 128
report['ink_semantics'] = 'Native editable ink contours; an in-memory insertion receipt is not a filesystem save receipt.'
report['hardware_limit'] = 'The ABI is compiled and gated against a foreign process; physical pen appearance/persistence requires the manual device check.'
for architecture, path in [('host', args.host_preview), ('arm64', args.arm_preview)]:
    shutil.copy2(path, args.output/f'preview-{architecture}.png')
(args.output/'validation.json').write_text(json.dumps(report, indent=2)+'\n')
print(json.dumps(report, indent=2))
