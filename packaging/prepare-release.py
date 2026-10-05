#!/usr/bin/env python3
"""Verify the public tree and assemble complete, checksum-addressed release assets."""
import argparse
import datetime
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import subprocess

PRIVATE_COMPONENTS = {'.superpowers', 'superpowers', 'planning', 'plans', '.tools', '.release-work'}
SECRET_PATTERNS = [re.compile(rb'-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----'),
                   re.compile(rb'gh[pousr]_[A-Za-z0-9]{30,}'),
                   re.compile(rb'(?<![A-Za-z0-9])sk-[A-Za-z0-9_-]{24,}')]


def git(*arguments):
    return subprocess.check_output(['git', *arguments])


def guard():
    files = git('ls-files', '-z').decode('utf-8').split('\0')
    for name in filter(None, files):
        parts = PurePosixPath(name).parts
        if (PRIVATE_COMPONENTS.intersection(parts)
                or any(part == 'build' or part.startswith('build-') or part == 'dist' for part in parts[:-1])):
            raise RuntimeError('Private/build file tracked in public source: ' + name)
        path = Path(name)
        if not path.is_file():
            raise RuntimeError('Tracked release input unavailable: ' + name)
        data = path.read_bytes()
        if any(pattern.search(data) for pattern in SECRET_PATTERNS):
            raise RuntimeError('Potential credential in public source: ' + name)
        if re.search(rb'/home/[a-zA-Z0-9._-]+/', data):
            raise RuntimeError('Private workstation path in public source: ' + name)
    for required in ['README.md', 'ROADMAP.md', 'CHANGELOG.md', 'LICENSE']:
        if required not in files:
            raise RuntimeError('Public release documentation missing: ' + required)
    print('PUBLIC_SOURCE_GUARD_PASS no private planning/tool/build files or detected credentials')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output')
    parser.add_argument('--version', default='0.1.0')
    parser.add_argument('--guard-only', action='store_true')
    args = parser.parse_args()
    guard()
    if args.guard_only:
        return
    if not args.output or not re.fullmatch(r'\d+\.\d+\.\d+', args.version):
        raise RuntimeError('Output directory and numeric semantic version required')
    configured = re.search(r'project\(RoseAgent VERSION ([0-9.]+)', Path('CMakeLists.txt').read_text())
    if not configured or configured.group(1) != args.version:
        raise RuntimeError('Release version differs from compiled CMake project version')
    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    for suffix, format_name in [('tar.gz', 'tar.gz'), ('zip', 'zip')]:
        destination = output / f'RoseAgent-{args.version}-source.{suffix}'
        subprocess.run(['git', 'archive', '--format=' + format_name,
                        '--prefix=RoseAgent-' + args.version + '/', '-o', str(destination), 'HEAD'], check=True)
    patterns = {
        'linuxDeb': '*.deb', 'linuxRpm': '*.rpm', 'linuxPortable': 'rose-agent-*-linux-*.tar.gz',
        'windowsX86Installer': '*-windows-x86-setup.exe',
        'windowsX64Installer': '*-windows-x64-setup.exe',
        'windowsX86Portable': '*-windows-x86-portable.zip',
        'windowsX64Portable': '*-windows-x64-portable.zip',
    }
    for kind, pattern in patterns.items():
        if len(list(output.glob(pattern))) != 1:
            raise RuntimeError('Exactly one verified asset required: ' + kind)
    for module in ['qtbase', 'qtsvg', 'qtimageformats']:
        for version, suffix in [('5.15.18', 'zip'), ('6.8.3', 'tar.xz')]:
            if not list(output.glob(f'{module}-everywhere-*{version}.{suffix}')):
                raise RuntimeError('Required corresponding Qt source absent: ' + module + ' ' + version)
    if not (output / 'qt5compat-everywhere-src-6.8.3.tar.xz').is_file():
        raise RuntimeError('Required Qt6 legacy-codec corresponding source absent')
    assets = []
    for path in sorted(output.iterdir()):
        if not path.is_file() or path.name in {'SHA256SUMS.txt', 'release-manifest.json'}:
            continue
        if path.stat().st_size == 0:
            raise RuntimeError('Empty release asset: ' + path.name)
        with path.open('rb') as stream:
            digest = hashlib.file_digest(stream, 'sha256').hexdigest()
        assets.append({'name': path.name, 'bytes': path.stat().st_size, 'sha256': digest})
    manifest = {
        'version': args.version, 'sourceCommit': git('rev-parse', 'HEAD').decode().strip(),
        'createdUtc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'prerelease': True, 'codeSigning': 'unsigned',
        'platforms': {
            'linux-x86_64': {'qt': '6.8.3', 'minimumGlibc': '2.35', 'buildHost': 'Ubuntu22.04',
                             'verification': 'hosted tests, actual staged/relocated GUI-CLI-codec-TLS and package metadata'},
            'windows-x64': {'qt': '6.8.3', 'minimumOs': 'Windows10', 'buildHost': 'WindowsServer2022',
                            'verification': 'hosted tests and actual deployed GUI-CLI-codec-TLS'},
            'windows-x86': {'qt': '5.15.18', 'minimumOsTarget': 'Windows8.1', 'toolset': 'MSVC2019/v14214.29',
                            'tls': 'Schannel', 'crt': 'app-localVC14214.29+SDK19041UCRT',
                            'verification': 'PE32/subsystem/API-floor audit and actual deployed hosted tests/GUI-CLI-codec-TLS',
                            'nativeWindows81GuestVerified': False},
        },
        'assets': assets,
    }
    manifest_path = output / 'release-manifest.json'
    manifest_path.write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    with manifest_path.open('rb') as stream:
        manifest_digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    checksums = ''.join(asset['sha256'] + '  ' + asset['name'] + '\n' for asset in assets)
    checksums += manifest_digest + '  release-manifest.json\n'
    (output / 'SHA256SUMS.txt').write_text(checksums, encoding='ascii')
    print('COMPLETE_RELEASE_MANIFEST_PASS packages/installers/portable/source/notices/checksums')


if __name__ == '__main__':
    main()
