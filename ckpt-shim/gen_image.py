#!/usr/bin/env python3
"""checkpoint -> bootable ELF32 image (shim + blob + memory pages).

The image is self-contained: no base ELF, no DPI, no simulator support.  Load it
in ooo_core/henry_tb, or on the FPGA, and it restores the checkpoint and runs.

Placement: checkpoint pages occupy 0x08000000.. so the shim+blob sit at PA
0x07000000, below every page and above the IP22 512KB low alias.  The shim reads
its blob through kseg1 (uncached) at the same PA, so nothing depends on cache
state at restore time.
"""
import struct, subprocess, sys, os
import ckpt, gen_shim

SHIM_PA   = 0x07000000
BLOB_PA   = 0x07010000
KSEG0     = 0x80000000
KSEG1     = 0xA0000000
AS        = "mips64-linux-gnuabi64-gcc"
OBJCOPY   = "mips64-linux-gnuabi64-objcopy"


def build_shim(c):
    asm = gen_shim.emit(c, KSEG1 | BLOB_PA)
    open("shim.S", "w").write(asm + "\n")
    subprocess.run([AS, "-march=mips3", "-mabi=64", "-EB", "-mno-abicalls",
                    "-fno-pic", "-nostdlib", "-c", "shim.S", "-o", "shim.o"], check=True)
    subprocess.run([OBJCOPY, "-O", "binary", "-j", ".text", "shim.o", "shim.bin"], check=True)
    return open("shim.bin", "rb").read()


def runs_of(pages):
    d = dict(pages)
    out, vas = [], sorted(d)
    start = prev = vas[0]
    for v in vas[1:]:
        if v == prev + 4096:
            prev = v; continue
        out.append((start, b"".join(d[a] for a in range(start, prev + 4096, 4096))))
        start = prev = v
    out.append((start, b"".join(d[a] for a in range(start, prev + 4096, 4096))))
    return out


def elf32_be(entry, segs):
    """segs = [(vaddr, bytes)] -> ELF32 big-endian MIPS executable."""
    EHSZ, PHSZ = 52, 32
    phoff = EHSZ
    off = phoff + PHSZ * len(segs)
    eh = struct.pack(">16sHHIIIIIHHHHHH",
                     b"\x7fELF\x01\x02\x01" + b"\x00" * 9,
                     2, 8, 1, entry, phoff, 0, 0, EHSZ, PHSZ, len(segs), 40, 0, 0)
    phs, body = b"", b""
    for va, data in segs:
        pad = (-len(body)) % 16
        body += b"\x00" * pad
        phs += struct.pack(">IIIIIIII", 1, off + len(body), va, va,
                           len(data), len(data), 5 | 2, 16)   # PF_R|W|X
        body += data
    return eh + phs + body


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit("usage: gen_image.py <checkpoint> <out.elf>")
    c = ckpt.Checkpoint(sys.argv[1])
    sys.stderr.write("[img] %s\n" % c.summary())

    shim = build_shim(c)
    blob = gen_shim.blob(c)

    segs = [(KSEG0 | SHIM_PA, shim), (KSEG0 | BLOB_PA, blob)]
    for pa, data in runs_of(c.pages):
        segs.append((KSEG0 | pa, data))

    # IP22 "System Memory Alias": the low 512 KB of RAM is ALSO visible at physical
    # 0, and that is where Linux puts its exception vectors (kseg0 0x80000000 ->
    # PA 0; the handler itself lives at 0x88000180 = PA 0x08000180).  r9999 does not
    # implement the alias -- henry_tb only survives by wrapping out-of-range PAs --
    # so without these mirror segments ooo_core fetches ZEROS at 0x80000180 and
    # retires a desert of nops on the first exception.
    RAM_BASE, ALIAS_LEN = 0x08000000, 0x80000
    alias = [(pa - RAM_BASE, d) for pa, d in c.pages
             if RAM_BASE <= pa < RAM_BASE + ALIAS_LEN]
    if alias:
        for pa, data in runs_of(alias):
            segs.append((KSEG0 | pa, data))
        sys.stderr.write("[img] IP22 memory alias: %d page(s) mirrored at PA 0\n" % len(alias))

    img = elf32_be(KSEG0 | SHIM_PA, segs)
    open(sys.argv[2], "wb").write(img)
    sys.stderr.write("[img] %s: %d segments, %.1f MB, entry=%08x shim=%dB blob=%dB\n"
                     % (sys.argv[2], len(segs), len(img) / 1e6,
                        KSEG0 | SHIM_PA, len(shim), len(blob)))
