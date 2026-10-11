#!/usr/bin/env python3
"""
flash_crc.py - patch the flash CRC word of the safety platform into an ELF.

The linker places the .flash_crc section (one 32-bit word) at the end of the
flash image. This script rebuilds the flash content from the ELF loadable
segments, computes the CRC exactly like the STM32 hardware CRC unit used by
safety_selftest.c (CRC-32/MPEG-2: poly 0x04C11DB7, init 0xFFFFFFFF, no
reflection, no final XOR, 32-bit little-endian words) over
[flash start, .flash_crc) and writes the result into the ELF file.

Usage:  python flash_crc.py <firmware.elf> [flash_start_hex]
CubeIDE post-build step (Project > Properties > C/C++ Build > Settings >
Build Steps > Post-build):
        python ../tools/flash_crc.py ${ProjName}.elf
"""

import struct
import sys

FLASH_START = 0x08000000   # STM32F746 flash origin
CRC_POLY = 0x04C11DB7
CRC_INIT = 0xFFFFFFFF
PT_LOAD = 1                # ELF program header type: loadable segment


def build_crc_table():
    """Byte-wise lookup table for the MSB-first CRC-32 (poly 0x04C11DB7)."""
    table = []                                  # 256 entries
    for byte in range(256):
        crc = byte << 24                        # byte in the top 8 bits
        for _ in range(8):
            crc = ((crc << 1) ^ CRC_POLY) if (crc & 0x80000000) else (crc << 1)
            crc &= 0xFFFFFFFF
        table.append(crc)
    return table


def stm32_crc(data):
    """CRC of data (multiple of 4 bytes) as computed by the STM32 CRC unit."""
    table = build_crc_table()                   # lookup table
    crc = CRC_INIT                              # running CRC
    for offset in range(0, len(data), 4):
        word = struct.unpack_from("<I", data, offset)[0]  # word as written to CRC->DR
        for shift in (24, 16, 8, 0):            # the unit processes the MSB first
            crc = ((crc << 8) & 0xFFFFFFFF) ^ table[((crc >> 24) ^ (word >> shift)) & 0xFF]
    return crc


def read_elf(elf):
    """Return (program headers, section headers by name) of an ELF32 LE file."""
    if elf[:4] != b"\x7fELF" or elf[4] != 1 or elf[5] != 1:
        sys.exit("flash_crc: not a 32-bit little-endian ELF file")

    e_phoff, e_shoff = struct.unpack_from("<II", elf, 0x1C)      # header table offsets
    e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHHHH", elf, 0x2A)

    segments = []                               # (type, file offset, paddr, file size)
    for index in range(e_phnum):
        p_type, p_offset, _vaddr, p_paddr, p_filesz = struct.unpack_from("<IIIII", elf, e_phoff + index * e_phentsize)
        segments.append((p_type, p_offset, p_paddr, p_filesz))

    raw_sections = []                           # (name offset, addr, file offset, size)
    for index in range(e_shnum):
        sh = struct.unpack_from("<IIIIII", elf, e_shoff + index * e_shentsize)
        raw_sections.append((sh[0], sh[3], sh[4], sh[5]))

    strtab_offset = raw_sections[e_shstrndx][2] # section name string table
    sections = {}                               # name -> (addr, file offset, size)
    for name_off, addr, offset, size in raw_sections:
        end = elf.index(b"\x00", strtab_offset + name_off)
        sections[elf[strtab_offset + name_off:end].decode()] = (addr, offset, size)
    return segments, sections


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    elf_path = sys.argv[1]                      # firmware to patch
    flash_start = int(sys.argv[2], 16) if len(sys.argv) > 2 else FLASH_START

    with open(elf_path, "rb") as handle:
        elf = bytearray(handle.read())          # whole ELF file

    segments, sections = read_elf(elf)
    if ".flash_crc" not in sections:
        sys.exit("flash_crc: no .flash_crc section (linker script not updated?)")
    crc_addr, crc_offset, crc_size = sections[".flash_crc"]
    if crc_size != 4 or crc_addr % 4:
        sys.exit("flash_crc: .flash_crc must be one aligned 32-bit word")

    # rebuild the flash image [flash_start, crc_addr) from the loadable segments
    length = crc_addr - flash_start             # bytes covered by the CRC
    image = bytearray(length)                   # flash content
    covered = bytearray(length)                 # 1 where a segment wrote a byte
    for p_type, p_offset, p_paddr, p_filesz in segments:
        if p_type != PT_LOAD or p_filesz == 0:
            continue
        start = max(p_paddr, flash_start)       # part of the segment inside the range
        end = min(p_paddr + p_filesz, crc_addr)
        if start >= end:
            continue
        src = p_offset + (start - p_paddr)      # file offset of the first byte
        image[start - flash_start:end - flash_start] = elf[src:src + (end - start)]
        covered[start - flash_start:end - flash_start] = b"\x01" * (end - start)

    # a gap would be 0xFF in flash but 0x00 in a .bin: refuse an ambiguous image
    if 0 in covered:
        gap = covered.index(0) + flash_start    # first uncovered address
        sys.exit("flash_crc: gap in the flash image at 0x%08X, CRC would be ambiguous" % gap)

    crc = stm32_crc(image)                      # expected CRC
    struct.pack_into("<I", elf, crc_offset, crc)
    with open(elf_path, "wb") as handle:
        handle.write(elf)
    print("flash_crc: 0x%08X over 0x%08X..0x%08X (%d bytes) -> %s"
          % (crc, flash_start, crc_addr, length, elf_path))


if __name__ == "__main__":
    main()
