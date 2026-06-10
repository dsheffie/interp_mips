# PROJECT_STATE — interp_mips (SGI Indy / IP22 functional simulator)

A MIPS III/IV functional simulator that boots SGI Indy (IP22) firmware/OSes and
can emit a retirement trace for the `../rv64analyzer` CFG/SSA tool.

## Retirement trace

Run with `--retiretrace <file>` to emit a boost-serialized `retire_trace`
(`std::list<inst_record{pc=physical, vpc=virtual, inst}>` + empty `tip` map),
consumed by `../rv64analyzer` (`./build/rv64analyzer -i <file>`). The hook is at
the top of `execMips<EL>` in `interpret.cc`, just after `s->icnt++`. Delay slots
fall out naturally (branches recurse into `execMips` for the slot); nullified
branch-likely slots are correctly absent. No cycle/timing info yet — that comes
from the CPU core later.

## IP / chip inventory (what an SGI Indy needs)

Register maps for the implemented chips are documented in-source (`sgi_mc.cc`,
`sgi_hpc.cc`) and cross-checked against the SGI chip specs in
`~/Desktop/indy_docs/`, Linux `arch/mips/.../sgi/*.h`, and NetBSD `arch/sgimips`.

Legend: ✅ functional · ⚠️ partial · ❌ stub/absent · ◻️ routing/loadable only

The **"What to model / skip"** column is the key planning column: most IPs only need
a thin register-mapped behavioral model (enough that PROM/OS polls succeed), not a
full chip. ✓ = implement; ✗ = can be ignored for a serial-console boot + trace.

### Core / memory / bus

| IP | Function | Spec | Status | What to model / skip |
|----|----------|------|--------|----------------------|
| R4600 / R4x00 (IP22 CPU) | MIPS III CPU+FPU, 16 KB I/D cache, TLB | R4300/R4400 manuals | ✅ `interpret.cc` | ✓ Full ISA + CP0/TLB/exceptions — this *is* the traced engine. ✗ cache timing |
| MC (Memory Controller) | DRAM cfg, refresh, RPSS counter, GIO64 arb, GIO DMA, serial-EEPROM ctrl | `mc.pdf` | ⚠️ `sgi_mc.cc` (regs documented; EEPROM/DMA stubbed) | ✓ Register file + reset constants (sysid; memcfg → RAM size; RPSS counter) + serial-EEPROM bit-bang. ✗ GIO-DMA engine |
| GIO64 bus | 64-bit I/O bus: CPU/mem ↔ HPC3 & graphics | `gio64.pdf` | ◻️ address routing | ✓ **Address routing only** + the few arb regs (already in MC). ✗ bus protocol / timing / arbitration / swizzle — the IP itself never needs to exist |

### I/O — HPC3 and its peripherals

| IP | Function | Spec | Status | What to model / skip |
|----|----------|------|--------|----------------------|
| HPC3 (first chip, base 0x1fb80000) | DMA engines, glue for SCSI/enet/EEPROM/PBUS, IRQ aggregation | `hpc3.pdf` | ⚠️ `sgi_hpc.cc` stubs | ✓ IRQ status (istat0/1), misc, PBUS PIO/DMA cfg round-trip, **+ SCSI DMA descriptor engine**. ✗ enet DMA (unless netboot), FIFO ports |
| IOC2 + INT2 | interrupt controller, timers, serial/kbd/parallel gating | `ioc.pdf` | ❌ (console is an MTC0-$7 shortcut) | ✓ INT2 interrupt latch/mask + system timer (OS tick) + serial routing. ✗ kbd/mouse, parallel |
| Serial — Zilog **Z85230 SCC** | console UART (chan B), behind IOC2/HPC3 | Zilog ESCC datasheet; Linux `ip22zilog.c` | ❌ (MTC0-$7 shortcut) | ✓ Functional chan B: data + status + Tx-empty/Rx-ready IRQ (real console I/O, replaces the hack). ✗ chan A, modem ctrl, SDLC |
| WD33C93A/B SCSI | Disk/CD controller (`dksc(0,…)`) | WD datasheet; Linux `wd33c93.c`/`sgiwd93.c` | ❌ logs only | ✓ **Functional SCSI** (biggest effort): cmd/status/data regs + phases for INQUIRY / READ CAPACITY / READ(10) / WRITE against a disk image, via HPC3 DMA. ✗ disconnect/reselect niceties |
| NMC93CS56 serial EEPROM | `eaddr` MAC + env vars; bit-banged | mc/hpc3; Linux `ip22-nvram.c` | ❌ **current boot blocker (MC 0x34)** | ✓ Serial bit-bang state machine (CS/CLK/DI/DO) + 128 B content with valid eaddr (`08:00:69:…`). Small |
| Seeq 8003 Ethernet | 10 Mb ethernet | Seeq datasheet; Linux `sgiseeq.c` | ❌ stub | ✗ Skip for disk boot. If netbooting: register model + HPC3 DMA + host tap |
| Dallas DS1286/DS1386 | RTC + battery NVRAM | Dallas datasheet; Linux `rtc-ds1286.c` | ❌ absent | ✓ TOD register reads + NVRAM byte array (holds PROM env). ✗ alarm / watchdog |
| Boot PROM | 512 KB firmware (`ip24prom*.bin`) | — | ◻️ loadable | ✓ Load blob / ARCS stub (done). ✗ nothing else |
| HAL2 audio / parallel (PBUS) | Iris Audio Processor A2, LPT | Linux `hal2.c` | ❌ absent | ✗ Skip. Optionally stub the ID/probe reg so detection fails cleanly |

### Graphics — "Newport" / XL board (NOT on the serial-console path)

| IP | Function | Spec | Status | What to model / skip |
|----|----------|------|--------|----------------------|
| REX3 | Raster engine (fills spans; CPU does triangle→span) | `rex3.pdf` | ❌ | ✗ None |
| VC2 | Video controller / timing / cursor | `vc2.pdf` | ❌ | ✗ None |
| XMAP9 | Cross-map / colormap / pixel mux | `xmap9.pdf` | ❌ | ✗ None |
| RB2 | RAM Buffer (framebuffer) | `rb2.pdf` | ❌ | ✗ None |
| RO1 | ReOrganizer (pixel reorg) | `ro1.pdf` | ❌ | ✗ None |
| DMUX1 | 36-bit MUX, board ↔ GIO64 | `dmux1.pdf` | ❌ | ✗ None |

For the whole graphics block, the only thing worth modeling is a **GIO slot probe
that reports "absent"** so the PROM falls back to the serial console instead of
hanging waiting for graphics.

## Critical path to a traceable boot

Minimum set: **CPU → MC → HPC3 → IOC2/INT2 (IRQs + timer) → Z85230 SCC (serial
console) → WD33C93 SCSI + disk → serial EEPROM (eaddr) → boot PROM/NVRAM.** The
entire graphics block can stay unimplemented as long as we boot to the serial
console (model only a GIO slot probe that reports graphics absent).

## Known issues / next steps

- **Next blocker:** serial EEPROM at MC reg `0x34` (NMC93xx bit-bang). MAME's
  eeprom holds the `eaddr` (SGI OUI `08:00:69:xx:xx:xx`).
- **MMIO byte-swap quirk:** device reads return host-native values but the load
  path bswaps for big-endian, so constant-valued registers read swapped (e.g.
  systemid=3 prints "Revision 0"). Harmless for the current boot; proper fix is
  to not byte-swap MMIO device reads/writes (only real memory).
- IP22 Linux (`vmlinux.32`) currently boots through MC system-id, GIO arbiter,
  and memory-config probing before the EEPROM stall. Run:
  `./interp_mips -f <kernel> --arcs <arcs_fw.bin> --retiretrace boot.rt -m <N>`
