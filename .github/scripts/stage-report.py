#!/usr/bin/env python3
"""Compare an ordinary and stripped install of the same pinned FEX build."""
import hashlib
import json
from pathlib import Path
import struct
import sys


def allocated_sections(data):
    assert data[:6] == b'\x7fELF\x02\x01', 'expected little-endian ELF64'
    offset = struct.unpack_from('<Q', data, 40)[0]
    stride, count, strings = struct.unpack_from('<HHH', data, 58)
    headers = [struct.unpack_from('<IIQQQQIIQQ', data, offset + i * stride) for i in range(count)]
    table = headers[strings]
    names = data[table[4]:table[4] + table[5]]
    result = {}
    for name, kind, flags, address, start, size, _, _, alignment, entry_size in headers:
        if flags & 2:  # SHF_ALLOC: instructions, data, relocations and runtime symbol tables.
            label = names[name:names.index(b'\0', name)].decode()
            digest = None if kind == 8 else hashlib.sha256(data[start:start + size]).hexdigest()
            result[label] = (kind, flags, address, size, alignment, entry_size, digest)
    return result


before, after = map(Path, sys.argv[1:])
left = {str(p.relative_to(before)): p for p in before.rglob('*') if p.is_file()}
right = {str(p.relative_to(after)): p for p in after.rglob('*') if p.is_file()}
assert left.keys() == right.keys(), 'stage file lists differ'
files = {}
for name, path in left.items():
    original, stripped = path.read_bytes(), right[name].read_bytes()
    if original.startswith(b'\x7fELF'):
        assert allocated_sections(original) == allocated_sections(stripped), name + ': runtime contents changed'
    else:
        assert original == stripped, name + ': non-ELF file changed'
    files[name] = {'before_bytes': len(original), 'after_bytes': len(stripped)}

installed = ['usr/bin/' + name for name in (
    'FEX', 'FEXBash', 'FEXGetConfig', 'FEXOfflineCompiler', 'FEXServer', 'FEXServerManager', 'FEXpidof')]
installed += ['FEXCompatTool', 'VERSIONS.txt', 'ConfigTemplate.json']
totals = {key: sum(files[name][key] for name in installed) for key in ('before_bytes', 'after_bytes')}
print(json.dumps({'installed_files': totals, 'all_allocated_sections_identical': True, 'files': files}, indent=2))
