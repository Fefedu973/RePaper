#!/usr/bin/env python3
"""Assemble the verified final PC download; never connect to a tablet."""
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
import zipfile

ROOT = Path(__file__).resolve().parents[2]
NAME = 'repaper-final-2026-09-06'
APPLOAD = ROOT / 'dist/appload-repaper-3.28.3'
REINK = ROOT / 'dist/reink-native-0.8.1'
AUDIT = ROOT / '.local/qa/final-release-audit'
DEST = ROOT / 'dist' / NAME
ZIP = ROOT / 'dist' / (NAME + '.zip')


def digest(path):
    with path.open('rb') as stream:
        sha = hashlib.sha256()
        for data in iter(lambda: stream.read(1024 * 1024), b''):
            sha.update(data)
    return sha.hexdigest()


def main():
    assert not DEST.exists() and not ZIP.exists(), 'Never overwrite a delivered package'
    old_hashes = {}
    for line in (APPLOAD / 'SHA256SUMS').read_text().splitlines():
        sha, relative = line.split('  ', 1)
        path = PurePosixPath(relative)
        assert not path.is_absolute() and '..' not in path.parts
        assert digest(APPLOAD / relative) == sha, relative
        old_hashes[relative] = sha
    audit = json.loads((AUDIT / 'final-release-audit.json').read_text())
    assert audit['status'] == 'PASS', 'Final source and package audit must pass'
    native = json.loads((AUDIT / 'native-package-verification.json').read_text())
    assert digest(REINK / 'reink-editor.so') == native['moduleSha256']
    assert digest(REINK / 'reink-native-0.8.1.tar.gz') == native['archiveSha256']
    DEST.mkdir()

    def copy(source, relative):
        target = DEST / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        assert source.is_file() and not source.is_symlink(), source
        shutil.copyfile(source, target)
        assert digest(source) == digest(target), relative

    for name in ('appload.so', 'repaper-apps-aarch64.tar.gz'):
        copy(APPLOAD / name, name)
    copy(REINK / 'reink-editor.so', 'reink-editor.so')
    copy(Path(__file__).with_name('README.md'), 'README.md')
    copy(Path(__file__), 'sources/package-final.py')
    copy(APPLOAD / 'README.md', 'docs/appload-3.28.3.md')
    copy(REINK / 'README.md', 'docs/reink-0.8.1.md')
    copy(APPLOAD / 'sources.tar.gz', 'sources/appload-3.28.3.tar.gz')
    copy(REINK / 'reink-native-0.8.1.tar.gz', 'sources/reink-native-0.8.1.tar.gz')
    copy(APPLOAD / 'manifest.json', 'validation/appload-component-manifest.json')
    copy(APPLOAD / 'SHA256SUMS', 'validation/appload-component-SHA256SUMS')
    for source in sorted((APPLOAD / 'validation').rglob('*')):
        if source.is_file():
            copy(source, 'validation/appload/' + source.relative_to(APPLOAD / 'validation').as_posix())
    for name in ('final-release-audit.json', 'native-package-verification.json'):
        copy(AUDIT / name, 'validation/' + name)

    files = {path.relative_to(DEST).as_posix():
             {'sha256': digest(path), 'bytes': path.stat().st_size}
             for path in sorted(DEST.rglob('*')) if path.is_file()}
    manifest = {
        'schemaVersion': 1, 'release': NAME, 'date': '2026-09-06',
        'target': 'Paper Pro ferrari OS 3.28.0.169', 'qt': '6.10.3',
        'components': {'appload': '3.28.3', 'nativeEditor': '0.8.1'},
        'apploadApplications': ['remoodle', 'reagenda', 'recalc'],
        'nativeTools': ['reInk', 'reSelect', 'reStencil'],
        'rmchatIncluded': False,
        'assemblyOnly': True,
        'binariesRebuiltForThisAssembly': False,
        'description': 'Consolidates byte-identical validated components after the source audit; original matching source archives retained.',
        'deviceInstallationPerformed': False,
        'deviceRuntimeValidatedByThisAssembly': False,
        'files': files,
    }
    (DEST / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    checksums = {relative: meta['sha256'] for relative, meta in files.items()}
    checksums['manifest.json'] = digest(DEST / 'manifest.json')
    (DEST / 'SHA256SUMS').write_text(''.join(f'{sha}  {relative}\n' for relative, sha in sorted(checksums.items())), encoding='utf-8')
    packaged = {**checksums, 'SHA256SUMS': digest(DEST / 'SHA256SUMS')}
    with zipfile.ZipFile(ZIP, 'x', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for relative in sorted(packaged):
            archive.write(DEST / relative, NAME + '/' + relative)
    with zipfile.ZipFile(ZIP) as archive:
        names = archive.namelist()
        assert len(names) == len(set(names)) == len(packaged)
        for name in names:
            path = PurePosixPath(name)
            assert not path.is_absolute() and '..' not in path.parts and '\\' not in name
            relative = str(path.relative_to(NAME))
            assert hashlib.sha256(archive.read(name)).hexdigest() == packaged[relative], relative
        assert archive.testzip() is None
    for relative, sha in old_hashes.items():
        assert digest(APPLOAD / relative) == sha, 'Earlier release changed: ' + relative
    sha = digest(ZIP)
    ZIP.with_suffix('.zip.sha256').write_text(f'{sha}  {ZIP.name}\n', encoding='utf-8')
    report = {'status': 'PASS', 'archive': str(ZIP), 'sha256': sha,
              'bytes': ZIP.stat().st_size, 'zipFilesVerified': len(packaged),
              'originalApploadChecksumEntriesUnchanged': len(old_hashes),
              'rmchatIncluded': False, 'deviceInstallationPerformed': False}
    (AUDIT / 'final-assembly-verification.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
