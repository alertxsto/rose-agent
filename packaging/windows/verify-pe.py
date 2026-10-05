#!/usr/bin/env python3
"""Reject wrong architecture/runtime or known post-8.1 static imports.

This is an artifact compatibility gate, NOT a substitute for real Win8.1 execution.
"""
import argparse
import json
from pathlib import Path
import pefile

POST_81_IMPORTS = {
    'GetDpiForWindow', 'GetDpiForSystem', 'GetSystemDpiForProcess',
    'AdjustWindowRectExForDpi', 'EnableNonClientDpiScaling',
    'SetThreadDpiAwarenessContext', 'SetProcessDpiAwarenessContext',
    'GetThreadDpiAwarenessContext', 'GetAwarenessFromDpiAwarenessContext',
    'SetThreadDescription', 'GetThreadDescription', 'GetSystemCpuSetInformation',
    'SetThreadSelectedCpuSets', 'GetThreadSelectedCpuSets',
    'SetProcessDefaultCpuSets', 'GetProcessDefaultCpuSets', 'IsWow64Process2',
    'GetTempPath2W', 'GetTempPath2A', 'CreatePseudoConsole',
    'ResizePseudoConsole', 'ClosePseudoConsole',
}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--directory', required=True)
    parser.add_argument('--architecture', choices=['x86', 'x64'], required=True)
    args = parser.parse_args()
    root = Path(args.directory).resolve()
    expected = 0x14c if args.architecture == 'x86' else 0x8664
    binaries = sorted(list(root.rglob('*.exe')) + list(root.rglob('*.dll')))
    if not (root / 'rose-agent.exe').is_file() or not (root / 'rose-cli.exe').is_file():
        raise RuntimeError('Actual GUI/CLI missing from stage')
    if args.architecture == 'x86':
        for required in ['Qt5Core.dll', 'Qt5Network.dll', 'vcruntime140.dll',
                         'msvcp140.dll', 'ucrtbase.dll', 'plugins/platforms/qwindows.dll',
                         'plugins/imageformats/qwebp.dll', 'plugins/printsupport/windowsprintersupport.dll']:
            if not (root / required).is_file():
                raise RuntimeError('Legacy runtime dependency missing: ' + required)
        if list(root.rglob('Qt6*.dll')):
            raise RuntimeError('Qt6 is not a Windows8.1/x86 runtime')
    report = []
    for binary in binaries:
        pe = pefile.PE(str(binary))
        try:
            if pe.FILE_HEADER.Machine != expected:
                raise RuntimeError(f'Wrong PE architecture: {binary}')
            minimum = (pe.OPTIONAL_HEADER.MajorSubsystemVersion,
                       pe.OPTIONAL_HEADER.MinorSubsystemVersion)
            # /SUBSYSTEM is the executable launch contract, not a DLL API floor.
            # Microsoft SDK app-local UCRT forwarders have 10.0 metadata while
            # Microsoft's deployment contract explicitly supports older Windows.
            # Audit architecture/imports for all DLLs; do not alter their headers.
            is_dll = bool(pe.FILE_HEADER.Characteristics & pefile.IMAGE_CHARACTERISTICS['IMAGE_FILE_DLL'])
            if args.architecture == 'x86' and not is_dll and minimum > (6, 3):
                raise RuntimeError(f'Post-Win8.1 executable subsystem floor {minimum}: {binary}')
            imports = []
            for table in ['DIRECTORY_ENTRY_IMPORT', 'DIRECTORY_ENTRY_DELAY_IMPORT']:
                for library in getattr(pe, table, []):
                    dll = library.dll.decode('ascii').lower()
                    for symbol in library.imports:
                        name = symbol.name.decode('ascii') if symbol.name else f'ordinal:{symbol.ordinal}'
                        imports.append(dll + ':' + name)
                        if args.architecture == 'x86' and name in POST_81_IMPORTS:
                            raise RuntimeError(f'Known post-Win8.1 static API import: {dll}:{name} in {binary}')
            report.append({'file': str(binary.relative_to(root)),
                           'machine': hex(expected), 'isDll': is_dll, 'subsystemVersion': list(minimum),
                           'imports': sorted(imports)})
        finally:
            pe.close()
    (root / 'pe-compatibility.json').write_text(json.dumps({
        'architecture': args.architecture,
        'scope': 'All PE architecture/imports; executable subsystem floor. DLL subsystem metadata is not an OS API contract. Native guest execution remains separate.',
        'binaries': report}, indent=2) + '\n', encoding='utf-8')
    print('PE_COMPATIBILITY_PASS', args.architecture, 'actual GUI/CLI/Qt/CRT/plugin closure inspected')


if __name__ == '__main__':
    main()
