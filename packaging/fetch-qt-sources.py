#!/usr/bin/env python3
"""Fetch official corresponding Qt sources; never package developer credentials."""
import argparse
import hashlib
from pathlib import Path
import shutil
import tarfile
import urllib.request
import zipfile

SOURCES = {
    '6.8.3': {
        'qtbase': '56001b905601bb9023d399f3ba780d7fa940f3e4861e496a7c490331f49e0b80',
        'qt5compat': '54b9c84bff34b423dd8c472862ce1009753ff505e418b4ef33907416da16b82e',
        'qtsvg': '35eb516460f00f264eb504baa253432384351cf23fb9980a5857190e8deef438',
        'qtimageformats': '049bfb99845e4801672aca07c3c4fc4c140f932a3a33faa899419579e33ef1c8',
        'qtwayland': '20fe385887d21190165a3180c17dcfc8b9a0e1da4ec76865b6334bdc709994b0',
    },
    '5.15.18': {
        'qtbase': 'b7218518b03f42a3dde075f27bfdef30e236b95f6aae58db763f4453f5ce6e7d',
        'qtsvg': '51e79736446896f264823436e38ff55b27d159d8c27b5d6601662cff381a44a9',
        'qtimageformats': 'c6389a389381e9e4eaec466cbe2902e4c39cb1a53cf52b3545f3118cc37d5eaa',
    },
}


def download(url, destination):
    for attempt in range(3):
        try:
            with urllib.request.urlopen(url, timeout=120) as response, destination.open('wb') as stream:
                shutil.copyfileobj(response, stream)
            return
        except Exception:
            destination.unlink(missing_ok=True)
            if attempt == 2:
                raise


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--version', choices=SOURCES, required=True)
    parser.add_argument('--output', required=True)
    parser.add_argument('--qt-prefix', help='Linux SDK prefix for exact ICU dependency notices')
    args = parser.parse_args()
    output = Path(args.output).resolve()
    sources = output / 'sources'
    archives = output / 'archives'
    sources.mkdir(parents=True, exist_ok=True)
    archives.mkdir(parents=True, exist_ok=True)
    major_minor = '.'.join(args.version.split('.')[:2])
    for module, expected in SOURCES[args.version].items():
        suffix = 'opensource-src-' + args.version + '.zip' if args.version.startswith('5.') else 'src-' + args.version + '.tar.xz'
        name = f'{module}-everywhere-{suffix}'
        archive = archives / name
        url = f'https://download.qt.io/archive/qt/{major_minor}/{args.version}/submodules/{name}'
        if not archive.exists():
            download(url, archive)
        if hashlib.file_digest(archive.open('rb'), 'sha256').hexdigest() != expected:
            raise RuntimeError(f'Official source SHA256 mismatch: {name}')
        directory = sources / module
        if not directory.exists():
            extraction = output / ('extract-' + module)
            extraction.mkdir(exist_ok=True)
            if name.endswith('.zip'):
                with zipfile.ZipFile(archive) as zipped:
                    for entry in zipped.infolist():
                        if not (extraction / entry.filename).resolve().is_relative_to(extraction.resolve()):
                            raise RuntimeError('Unsafe source archive path')
                    zipped.extractall(extraction)
            else:
                with tarfile.open(archive) as tar:
                    tar.extractall(extraction, filter='data')
            roots = list(extraction.iterdir())
            if len(roots) != 1 or not roots[0].is_dir():
                raise RuntimeError('Unexpected official source archive layout')
            shutil.move(str(roots[0]), directory)
            extraction.rmdir()
        print(f'CORRESPONDING_SOURCE {module} {args.version} sha256={expected}')
    if args.qt_prefix:
        prefix = Path(args.qt_prefix).resolve()
        extra = output / 'extra-notices'
        libraries = sorted({path.resolve().name for path in (prefix / 'lib').glob('libicu*.so*') if path.is_file()})
        if any(not name.endswith('.73.2') for name in libraries):
            raise RuntimeError('Unrecognized SDK ICU version; do not attach an incorrect license/source notice')
        extra.mkdir(exist_ok=True)
        (extra / 'LICENSES').mkdir(exist_ok=True)
        if libraries:
            revision = '680f521746a3bd6a86f25f25ee50a62d88b489cf'
            download(f'https://raw.githubusercontent.com/unicode-org/icu/{revision}/icu4c/LICENSE',
                     extra / 'LICENSES' / 'ICU-73.2.txt')
            (extra / 'SOURCE.txt').write_text(
                'ICU73.2 supplied by the official Qt6.8.3 Linux SDK, dynamically linked and unmodified.\n'
                f'Corresponding source: https://github.com/unicode-org/icu/tree/{revision}/icu4c\n'
                'Build provenance: official download.qt.io Qt6.8.3 gcc_64 binary distribution.\n', encoding='utf-8')
            (extra / 'NOTICE.txt').write_text('ICU73.2: Unicode and IBM copyrights; see LICENSES/ICU-73.2.txt.\n', encoding='utf-8')
        else:
            (extra / 'SOURCE.txt').write_text('No additional ICU shared libraries found in SDK.\n', encoding='utf-8')
            (extra / 'NOTICE.txt').write_text('No additional SDK shared libraries are covered by this directory.\n', encoding='utf-8')
        (extra / 'Libraries.txt').write_text(''.join(name + '\n' for name in libraries), encoding='utf-8')
    print('Qt corresponding-source root:', sources)


if __name__ == '__main__':
    main()
