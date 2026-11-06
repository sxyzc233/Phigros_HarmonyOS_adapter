#!/usr/bin/env python3
"""Check the Git index (not ignored local inputs) before a public commit."""
from pathlib import Path
import re
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]
ALLOWED={'.md','.cpp','.c','.h','.hpp','.inc','.txt','.ets','.ts','.json','.json5','.py','.ps1','.svg'}
RULES={
    'private key':rb'-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----',
    'GitHub token':rb'(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{40,})',
    'signing password':rb'["\x27](?:storePassword|keyPassword)["\x27]\s*:\s*["\x27][^"\x27]+',
    'local user path':rb'(?:[A-Za-z]:[\\/]+Users[\\/]+[^\\/\s"\x27]+|/home/[A-Za-z0-9_-]+/)',
    'literal account credential':rb'["\x27](?:access_token|mac_key|sessionToken|refresh_token)["\x27]\s*:\s*["\x27][A-Za-z0-9+/=_-]{12,}',
}
def check():
    names=subprocess.check_output(['git','ls-files','--cached','-z'],cwd=ROOT).decode('utf-8').split('\0')
    failures=[];count=0;total=0
    for name in filter(None,names):
        p=Path(name);count+=1
        if p.is_absolute() or '..' in p.parts:failures.append((name,'unsafe path'));continue
        if p.suffix.lower() not in ALLOWED and p.name not in {'LICENSE','.gitignore','.gitattributes'}:
            failures.append((name,'unexpected file type'));continue
        if any(part in {'build','libs','rawfile','signing','node_modules','oh_modules','.hvigor','.hvigor-user'} for part in p.parts):
            failures.append((name,'private/generated directory'));continue
        # Scan the actual staged bytes, including changes that differ locally.
        data=subprocess.check_output(['git','show',':'+name],cwd=ROOT)
        total+=len(data)
        if len(data)>2*1024*1024 or b'\0' in data:failures.append((name,'binary/oversized file'))
        for label,pattern in RULES.items():
            if re.search(pattern,data):failures.append((name,label))
    for name,label in failures:print(f'FAIL {name}: {label}')
    if not count:print('FAIL empty Git index');return 1
    if failures:return 1
    print(f'PASS staged public tree: {count} text/source files, {total} bytes; no matched private data or proprietary binaries.')
    print('Heuristic scan only; review source provenance and changes as well.')
    return 0
if __name__=='__main__':sys.exit(check())
