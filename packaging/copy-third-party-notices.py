#!/usr/bin/env python3
"""Assemble real license texts from matching official Qt module sources."""
import argparse
import json
from pathlib import Path
import shutil


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--sources', required=True)
    parser.add_argument('--destination', required=True)
    parser.add_argument('--version', choices=['5.15.18', '6.8.3'], required=True)
    args = parser.parse_args()
    root = Path(args.sources).resolve()
    destination = Path(args.destination).resolve()
    destination.mkdir(parents=True, exist_ok=True)
    modules = ['qtbase', 'qtsvg', 'qtimageformats']
    if args.version.startswith('6.'):
        modules.append('qt5compat')
    if args.version.startswith('6.') and (root / 'qtwayland').is_dir():
        modules.append('qtwayland')
    copied = []
    for module in modules:
        candidates = [root / module, root / f'{module}-everywhere-src-{args.version}']
        source = next((path for path in candidates if path.is_dir()), None)
        if source is None:
            raise RuntimeError('Matching official source module missing: ' + module)
        config = source / ('.qmake.conf' if args.version.startswith('5.') else '.cmake.conf')
        if not config.is_file() or args.version not in config.read_text(encoding='utf-8'):
            raise RuntimeError('Exact module source version mismatch: ' + module)
        license_paths = []
        for path in source.rglob('*'):
            if not path.is_file():
                continue
            relative = path.relative_to(source)
            filename = path.name.upper()
            if (filename.startswith(('LICENSE', 'LICENCE', 'COPYING', 'COPYRIGHT', 'NOTICE'))
                    or filename == 'QT_ATTRIBUTION.JSON'
                    or 'LICENSES' in [part.upper() for part in relative.parts]):
                license_paths.append(path)
        if not any(path.name.upper().startswith(('LICENSE.LGPL', 'LGPL')) or 'LGPL-3.0' in path.name.upper() for path in license_paths):
            raise RuntimeError('Actual LGPL text missing: ' + module)
        for path in sorted(license_paths):
            relative = Path(module) / path.relative_to(source)
            target = destination / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, target)
            copied.append(str(relative))
    series = '.'.join(args.version.split('.')[:2])
    suffix = f'opensource-src-{args.version}.zip' if args.version.startswith('5.') else f'src-{args.version}.tar.xz'
    urls = [f'https://download.qt.io/archive/qt/{series}/{args.version}/submodules/{module}-everywhere-{suffix}' for module in modules]
    (destination / 'Qt-NOTICE.txt').write_text(
        f'Qt{args.version} shared libraries/plugins are dynamically linked, unmodified official source/binaries.\n'
        'Qt and its included third-party components retain their respective copyrights/licenses.\n'
        'Actual module license texts and third-party notices are in the corresponding module directories.\n'
        'Corresponding source archives are also attached to this GitHub release; direct official locations:\n'
        + ''.join(url + '\n' for url in urls)
        + 'Build/deployment scripts are provided in the public Rose Agent source archive.\n'
        'You may replace the Qt shared libraries with compatible modified libraries for your own use.\n'
        'No restriction on reverse engineering for debugging modifications to the LGPL components is imposed.\n',
        encoding='utf-8')
    (destination / 'notice-manifest.json').write_text(json.dumps({
        'qtVersion': args.version, 'correspondingSources': urls, 'licenseFiles': copied
    }, indent=2) + '\n', encoding='utf-8')
    print('QT_NOTICES_PASS exact-version LGPL/third-party texts and corresponding-source locations', args.version)


if __name__ == '__main__':
    main()
