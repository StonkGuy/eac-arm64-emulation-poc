#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tiny static linker for ONE freestanding x86-64 relocatable object -> static ELF executable.

Exists so the freestanding test programs in tests/ can be built on an aarch64 machine that has clang (with the x86-64
target) but no x86-64 linker (the LLVM builds that ship with many distros can compile for x86-64 but their ld.lld often
cannot be used, and binutils is aarch64-only). It is NOT a general linker: one input object, no libraries, no shared
objects, no TLS, no GOT/PLT; only the relocation types clang emits for -fno-pic -ffreestanding code are handled.

usage: static-link.py input.o output
"""
import os
import struct
import sys

BASE = 0x400000


def main(argv):
    if len(argv) != 3:
        sys.exit(__doc__)
    obj = open(argv[1], "rb").read()
    out = argv[2]
    if obj[:4] != b"\x7fELF" or obj[4] != 2 or struct.unpack_from("<H", obj, 0x12)[0] != 0x3E:
        sys.exit("input must be an x86-64 ELF64 relocatable object")

    shoff = struct.unpack_from("<Q", obj, 0x28)[0]
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", obj, 0x3A)
    sections = []
    for i in range(shnum):
        name, typ, flags, _addr, off, size, link, info, align, _entsize = struct.unpack_from("<IIQQQQIIQQ", obj, shoff + i * shentsize)
        sections.append(dict(name=name, type=typ, flags=flags, off=off, size=size, link=link, info=info, align=align))

    def cstr(table, idx):
        end = table.index(b"\0", idx)
        return table[idx:end].decode()

    shstr = obj[sections[shstrndx]["off"]:sections[shstrndx]["off"] + sections[shstrndx]["size"]]
    for s in sections:
        s["sname"] = cstr(shstr, s["name"])

    symtab = next(s for s in sections if s["type"] == 2)
    strtab_sec = sections[symtab["link"]]
    strtab = obj[strtab_sec["off"]:strtab_sec["off"] + strtab_sec["size"]]
    symbols = []
    for i in range(symtab["size"] // 24):
        name, _info, _other, shndx, value, _size = struct.unpack_from("<IBBHQQ", obj, symtab["off"] + i * 24)
        symbols.append(dict(name=cstr(strtab, name), shndx=shndx, value=value))

    # Layout: every SHF_ALLOC section, in order, after a 4 KiB header page. One RWX PT_LOAD maps the whole image.
    header = 0x1000
    cur = header
    place = {}
    order = [i for i, s in enumerate(sections) if s["flags"] & 2 and s["type"] in (1, 14, 15, 16)]  # PROGBITS, *_ARRAY
    order += [i for i, s in enumerate(sections) if s["flags"] & 2 and s["type"] == 8]               # NOBITS (.bss) last
    for i in order:
        s = sections[i]
        align = max(s["align"], 1)
        cur = (cur + align - 1) // align * align
        place[i] = cur
        cur += s["size"]
    filesz = max([place[i] + sections[i]["size"] for i in place if sections[i]["type"] != 8] or [header])
    memsz = cur
    image = bytearray(filesz)  # .bss (NOBITS) is not stored in the file; the PT_LOAD memsz covers it
    for i, addr in place.items():
        s = sections[i]
        if s["type"] != 8:
            image[addr:addr + s["size"]] = obj[s["off"]:s["off"] + s["size"]]

    def sym_addr(k):
        sym = symbols[k]
        if sym["shndx"] == 0xFFF1:  # SHN_ABS
            return sym["value"]
        if sym["shndx"] not in place:
            sys.exit("undefined or non-allocated symbol: %s" % sym["name"])
        return BASE + place[sym["shndx"]] + sym["value"]

    for rel in sections:
        if rel["type"] != 4:  # SHT_RELA
            continue
        target = rel["info"]
        if target not in place:
            continue
        for j in range(rel["size"] // 24):
            r_off, r_info, addend = struct.unpack_from("<QQq", obj, rel["off"] + j * 24)
            sym, typ = r_info >> 32, r_info & 0xFFFFFFFF
            loc = place[target] + r_off
            pc = BASE + loc
            val = sym_addr(sym)
            if typ in (2, 4):      # R_X86_64_PC32, PLT32
                struct.pack_into("<i", image, loc, val + addend - pc)
            elif typ == 1:         # R_X86_64_64
                struct.pack_into("<Q", image, loc, val + addend)
            elif typ == 10:        # R_X86_64_32
                struct.pack_into("<I", image, loc, (val + addend) & 0xFFFFFFFF)
            elif typ == 11:        # R_X86_64_32S
                struct.pack_into("<i", image, loc, val + addend)
            else:
                sys.exit("unsupported relocation type %d" % typ)

    start = next((s for s in symbols if s["name"] == "_start" and s["shndx"] in place), None)
    if start is None:
        sys.exit("no _start symbol")
    entry = BASE + place[start["shndx"]] + start["value"]

    ehdr = b"\x7fELF\x02\x01\x01\0" + b"\0" * 8 + struct.pack("<HHIQQQIHHHHHH", 2, 0x3E, 1, entry, 64, 0, 0, 64, 56, 1, 0, 0, 0)
    phdr = struct.pack("<IIQQQQQQ", 1, 7, 0, BASE, BASE, len(image), memsz, 0x1000)  # PT_LOAD, RWX
    image[0:len(ehdr)] = ehdr
    image[64:64 + len(phdr)] = phdr
    with open(out, "wb") as f:
        f.write(image)
    os.chmod(out, 0o755)
    print("linked %s (%d bytes, entry 0x%x)" % (out, len(image), entry))


if __name__ == "__main__":
    main(sys.argv)
