#!/usr/bin/env python3
"""Reports how much static RAM (DRAM) a firmware build leaves, and fails when it's too little.

The ESP32's static data and bss live in the dram0_0_seg region, which ends at 0x3FFDC200; the linker
puts the symbol _heap_start where they end. Whatever is left between the two is all the room there
is for more static buffers, and when it runs out the link fails with "dram0_0_seg overflowed" (the
"RAM: x%" line PlatformIO prints counts against a bigger total, so it doesn't warn). The heap is a
separate matter: the clock reports its free heap at run time (Info page, MQTT).

    python3 tools/check_ram.py .pio/build/ipstube/firmware.elf [--min 2048]
"""
import argparse
import struct
import sys

DRAM0_END = 0x3FFB0000 + 0x2C200


def read_symbols(path):
    """All symbols as (name, value, size)."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        sys.exit(f"{path}: not a 32-bit little-endian ELF file")
    shoff, = struct.unpack_from("<I", data, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x2E)
    sections = [struct.unpack_from("<IIIIIIIIII", data, shoff + i * shentsize) for i in range(shnum)]
    out = []
    for sh in sections:
        if sh[1] != 2:          # SHT_SYMTAB
            continue
        strtab = sections[sh[6]]
        for off in range(sh[4], sh[4] + sh[5], sh[9]):
            name_off, value, size = struct.unpack_from("<III", data, off)
            start = strtab[4] + name_off
            name = data[start:data.index(b"\0", start)].decode("ascii", "replace")
            out.append((name, value, size))
    return out


def symbols(path, wanted):
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        sys.exit(f"{path}: not a 32-bit little-endian ELF file")
    shoff, = struct.unpack_from("<I", data, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x2E)
    sections = [struct.unpack_from("<IIIIIIIIII", data, shoff + i * shentsize) for i in range(shnum)]
    found = {}
    for sh in sections:
        if sh[1] != 2:          # SHT_SYMTAB
            continue
        strtab = sections[sh[6]]
        for off in range(sh[4], sh[4] + sh[5], sh[9]):
            name_off, value = struct.unpack_from("<II", data, off)
            start = strtab[4] + name_off
            name = data[start:data.index(b"\0", start)].decode("ascii", "replace")
            if name in wanted:
                found[name] = value
    return found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("--min", type=int, default=2048, help="least headroom in bytes before failing")
    ap.add_argument("--top", type=int, default=0, help="also list the N biggest static variables")
    args = ap.parse_args()
    syms = symbols(args.elf, {"_heap_start", "_data_start", "_bss_end"})
    if "_heap_start" not in syms:
        sys.exit(f"{args.elf}: no _heap_start symbol")
    headroom = DRAM0_END - syms["_heap_start"]
    print(f"static RAM left: {headroom} bytes (static data ends at 0x{syms['_heap_start']:08X}, "
          f"the region at 0x{DRAM0_END:08X})")
    if args.top:
        start = 0x3FFB0000
        big = sorted({(n, v, sz) for n, v, sz in read_symbols(args.elf)
                      if start <= v < syms["_heap_start"] and sz > 0}, key=lambda t: -t[2])
        for name, value, size in big[:args.top]:
            print(f"  {size:7d}  {name}")
    if headroom < args.min:
        print(f"::error::Only {headroom} bytes of static RAM left (at least {args.min} wanted). "
              "Put big buffers on the heap, or allocate them when they're used.")
        sys.exit(1)


if __name__ == "__main__":
    main()
