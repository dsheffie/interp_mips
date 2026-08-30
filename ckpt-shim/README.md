# ckpt-shim -- full-system checkpoint replay for r9999

Turns an `interp_mips` checkpoint into a self-contained big-endian MIPS ELF that
restores itself: no back-door state loader, no DPI. A shim prologue programs the
platform (48 TLB entries, CP0, FP, HI/LO, GPRs) using ordinary instructions and
`eret`s into the captured PC, so the same image runs in `interp_mips`, `ooo_core`,
and on silicon.

    ./interp_mips -f vmlinux.32 --prom henry_arcs.bin --start-pc 0xbfc00000 \
        --disk debian.img --checkpoint-icnt <N> --checkpoint-out ck.bin
    python3 gen_image.py ck.bin ck.elf
    ./ooo_core -f ck.elf --maxicnt 5000000

## Things that are easy to get wrong (each cost a debug cycle)

* **Blob endianness.** The checkpoint file is little-endian (written by an x86
  host); the MIPS target reads the blob big-endian. Pack with `>`.
* **Status.EXL.** The final `mtc0` must keep EXL SET, or the shim drops to user
  mode four instructions before its own `eret` and the next kseg0 fetch takes an
  AdEL. `eret` clears EXL and lands in the checkpoint's mode. (A checkpoint taken
  with EXL already set cannot be expressed this way.)
* **IP22 System Memory Alias.** The low 512 KB of RAM is also visible at physical
  0, which is where Linux puts its BEV=0 exception vectors (kseg0 0x80000000; the
  handler itself lives at 0x88000180). r9999 does not implement the alias --
  `henry_tb` only survives by wrapping out-of-range PAs -- so `gen_image.py`
  mirrors those pages. Without it the core fetches zeros at 0x80000180 and
  retires a desert of `nop`s on the first exception.
* **FP restore needs CU1** enabled in the working Status, and `.set fp=64` for
  odd `ldc1` targets under FR=0.

## Co-sim notes

`ooo_core -c 1` needs top.cc's resync to know the BEV=0 vectors. Dhrystone
checkpoints co-sim clean (821k GPR checks); binutils checkpoints still diverge on
`ra` after the first exception -- the post-exception resync heuristic, not fixed.
