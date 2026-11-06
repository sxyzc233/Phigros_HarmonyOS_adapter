#!/usr/bin/env python3
"""Prepare ignored, local build inputs from a user-supplied Android APK."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
FINGERPRINTS = {
    'lib/arm64-v8a/libunity.so': (20253848, 0x1885092e),
    'lib/arm64-v8a/libil2cpp.so': (71737064, 0xb933272b),
    'lib/arm64-v8a/libmain.so': (6728, 0x581468ea),
    'lib/arm64-v8a/libnativeaudioe7.so': (108544, 0xcdc4fc99),
    'assets/bin/Data/Managed/Metadata/global-metadata.dat': (11287864, 0x252444cb),
    'assets/bin/Data/boot.config': (151, 0x65789d08),
}

def checked_archive(archive, bootstrap):
    names = archive.namelist()
    if len(names) != len(set(names)):
        raise ValueError('APK has duplicate member names')
    for name, expected in FINGERPRINTS.items():
        info = archive.getinfo(name)
        if (info.file_size, info.CRC) != expected:
            raise ValueError(f'Unsupported APK: {name}; expected Android 4.0.1.2')
    for name in bootstrap:
        path = PurePosixPath(name)
        if path.is_absolute() or '..' in path.parts or '\\' in name or not name.startswith('assets/bin/Data/'):
            raise ValueError('Unsafe bootstrap path')
        archive.getinfo(name)

def patch_audio(data):
    old, new = b'libOpenSLES.so\0', b'libReaSLES.so\0\0'
    if data.count(old) != 1:
        raise ValueError('Unexpected native-audio dependency layout')
    # Same-sized ELF string-table change: use the Harmony audio bridge.
    return data.replace(old, new)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('apk', type=Path)
    parser.add_argument('--check', action='store_true', help='validate identity and paths without extracting')
    args = parser.parse_args()
    bootstrap = json.loads((ROOT/'tools/bootstrap-files.json').read_text(encoding='utf-8'))
    with zipfile.ZipFile(args.apk) as archive:
        checked_archive(archive, bootstrap)
        patch_audio(archive.read('lib/arm64-v8a/libnativeaudioe7.so'))
        if args.check:
            print(f'APK identity/path checks passed: Android 4.0.1.2, {len(bootstrap)} bootstrap files')
            return
        # Read every selected member to EOF, so zipfile checks its actual CRC.
        # Only allowlisted paths are extracted, never arbitrary ZIP paths.
        for index, name in enumerate(list(FINGERPRINTS)[:4] + bootstrap):
            if name.startswith('lib/'):
                destination = ROOT/'entry/libs/arm64-v8a'/PurePosixPath(name).name
            else:
                destination = ROOT/'entry/src/main/resources/rawfile'/name
            destination.parent.mkdir(parents=True, exist_ok=True)
            with tempfile.NamedTemporaryFile(dir=destination.parent, delete=False) as out:
                temporary = Path(out.name)
                try:
                    if name.endswith('/libnativeaudioe7.so'):
                        out.write(patch_audio(archive.read(name)))
                    else:
                        with archive.open(name) as source:
                            shutil.copyfileobj(source, out, 2*1024*1024)
                except BaseException:
                    out.close(); temporary.unlink(missing_ok=True); raise
            temporary.replace(destination)
            if index % 100 == 0:
                print(f'Preparing local inputs {index+1}/{len(bootstrap)+4}', flush=True)
    print('Prepared ignored local inputs. Do not commit or redistribute these files.')

if __name__ == '__main__':
    main()
