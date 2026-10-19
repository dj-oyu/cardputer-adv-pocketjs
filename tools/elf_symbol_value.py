"""Read one defined 32-bit object from an ELF32/ELF64 image without SDK tools."""

import struct


def symbol_u32(elf: bytes, wanted: str) -> int:
    if elf[:4] != b'\x7fELF' or elf[5] != 1 or elf[4] not in (1, 2):
        raise ValueError('expected little-endian ELF32/ELF64')
    is_64 = elf[4] == 2
    shoff = struct.unpack_from('<Q' if is_64 else '<I', elf, 40 if is_64 else 32)[0]
    shentsize, shnum = struct.unpack_from('<HH', elf, 58 if is_64 else 46)
    shfmt = '<IIQQQQIIQQ' if is_64 else '<IIIIIIIIII'
    symfmt = '<IBBHQQ' if is_64 else '<IIIBBH'
    if shentsize < struct.calcsize(shfmt) or not shnum or shoff + shentsize * shnum > len(elf):
        raise ValueError('invalid ELF section table')
    sections = [struct.unpack_from(shfmt, elf, shoff + i * shentsize) for i in range(shnum)]

    def area(start, size):
        if start < 0 or size < 0 or start + size > len(elf):
            raise ValueError('ELF section outside image')
        return elf[start:start + size]

    found = []
    for sec in sections:
        if sec[1] != 2:  # SHT_SYMTAB
            continue
        link, entsize = sec[6], sec[9]
        if link >= len(sections) or entsize < struct.calcsize(symfmt) or sec[5] % entsize:
            raise ValueError('invalid ELF symbol table')
        names_sec = sections[link]
        names = area(names_sec[4], names_sec[5])
        symbols = area(sec[4], sec[5])
        for pos in range(0, len(symbols), entsize):
            sym = struct.unpack_from(symfmt, symbols, pos)
            name, info, index, value, size = ((sym[0], sym[1], sym[3], sym[4], sym[5]) if is_64
                                               else (sym[0], sym[3], sym[5], sym[1], sym[2]))
            if name >= len(names):
                raise ValueError('invalid ELF symbol name')
            end = names.find(b'\0', name)
            if end < 0:
                raise ValueError('unterminated ELF symbol name')
            if names[name:end] != wanted.encode():
                continue
            if info & 15 != 1 or size != 4 or index >= len(sections):
                raise ValueError('comparison seed is not a defined 32-bit object')
            target = sections[index]
            if target[1] == 8:  # SHT_NOBITS cannot prove an initialized value.
                raise ValueError('comparison seed has no stored ELF bytes')
            delta = value - target[3]
            if delta < 0 or delta + 4 > target[5]:
                raise ValueError('comparison seed is outside its ELF section')
            found.append(struct.unpack('<I', area(target[4] + delta, 4))[0])
    if len(found) != 1:
        raise ValueError('comparison seed ELF symbol missing or ambiguous')
    return found[0]
