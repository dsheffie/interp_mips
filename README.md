# interp_mips

A fast **functional** MIPS (R4000 / SGI IP22) instruction-set simulator. It boots
SGI IP22 firmware + kernels (IRIX 6.5, IP22 Linux) using a **1:1 virtual→physical
mapping** (no real TLB datapath), which sidesteps the RTL's TLB wall and makes it
a convenient oracle / co-sim reference for the r9999 core and the henry SoC, and
the place to get the **HPC3 SCSI-DMA + WD33C93 + disk** model right (`sgi_scsi.cc`).

## Build

```sh
make clean && make      # NOTE: no dependency tracking -- always `make clean` first
```

## Boot IRIX 6.5

```sh
./interp_mips \
  -f         /home/dsheffie/code/chd-dumper/extracted/unix \
  --prom     /home/dsheffie/code/r9999/arcs/henry_arcs.bin \
  --start-pc 0xbfc00000 \
  --disk     /home/dsheffie/code/chd-dumper/irix65.img \
  -m 60000000
```

Reaches the IRIX banner (clean -- no unimplemented ops):

```
r9999 PROM: CPU alive, handing off to the kernel
IRIX Release 6.5 IP22 Version 10070055 System V
Copyright 1987-2003 Silicon Graphics, Inc.
```

`--disk` backs the SCSI root device (the HPC3 SCSI-DMA + WD33C93 + disk model in
`sgi_scsi.cc`); without it IRIX can't mount root. Getting past "Enter Root
device" to a mounted root is the work in progress.

## Boot IP22 Linux

```sh
./interp_mips \
  -f         /home/dsheffie/code/linux-mips/vmlinux.32 \
  --prom     /home/dsheffie/code/r9999/arcs/henry_arcs.bin \
  --start-pc 0xbfc00000 \
  -m 120000000
```

Linux uses an initramfs (`/init`), so no `--disk` is needed.

**Status (WIP):** the current kernel boots with `console=arc`, so after the PROM
hands off, Linux calls *back* into the ARC romvec `putchar` in the firmware --
which currently hits an unimplemented instruction in the simulator and spins, so
console output doesn't appear yet. (interp_mips previously booted IP22 Linux to
the VFS root mount via the built-in pseudo-BIOS -- git `f40202f` -- before the
kernel switched to `console=arc`.) IRIX is unaffected because it drives its own
SCC console directly rather than the PROM romvec.

## Firmware / boot loaders

- **`--prom <blob> --start-pc 0xbfc00000`** *(recommended)* -- load a flat FSBL
  blob (e.g. `r9999/arcs/henry_arcs.bin`) at phys `0x1fc00000` and start at the
  reset vector. This mirrors exactly how `henry_tb` and the FPGA boot.
- *(no `--prom` / `--start-pc`)* -- the built-in **pseudo-BIOS** synthesizes the
  ARCS argv/envp handoff. Older path; the firmware-blob form above is preferred.
- **`--arcs <blob>`** -- load an ARCS blob at PA `0x1000` (legacy stub scheme).

## Common options

| flag | meaning |
|------|---------|
| `-f <elf>` | kernel ELF (IRIX `/unix`, Linux `vmlinux.32`) |
| `--prom <blob>` | flat FSBL firmware loaded at phys `0x1fc00000` (use with `--start-pc 0xbfc00000`) |
| `--start-pc <pc>` | start PC; skips the pseudo-BIOS (the firmware does the handoff) |
| `--disk <img>` | raw SCSI disk image for HD0 (IRIX root, e.g. `irix65.img`) |
| `-m`, `--maxicnt <n>` | cap on instructions executed |

Paths above are the local checkout locations; adjust for your tree. The IP22
firmware blob `henry_arcs.bin` is built in `r9999/arcs/` (`make`).
