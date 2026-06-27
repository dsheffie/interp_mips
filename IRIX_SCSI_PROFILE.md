# IRIX 6.5 SCSI / DMA profile — power-on → console prompt

Profile of every SCSI command and DMA transfer the IRIX 6.5.22 boot issues to
the `interp_mips` WD33C93 model, from PROM power-on through to the interactive
serial-console shell (`IRIS 1#`). Captured 2026-06-26 on the clean image. This
doc is written so another session can **reproduce, regenerate, or extend** the
numbers.

> Companion doc: `IRIX_POSTLOGIN_DEBUG.md` (boot recipe, gdb stub, kernel
> offsets). SCSI model lives in `sgi_scsi.cc`; HPC3 DMA channel in `sgi_hpc.cc`.

---

## 1. How to capture (reproducible)

The SCSI model logs every decoded CDB and every selection-timeout to **stderr**
when `SCSIDBG=1` (gated `SPRINTF` macro, `sgi_scsi.cc:10`). Boot with stdin
fed from a regular file (appends are picked up after EOF), capture stderr:

```bash
cd /home/dsheffie/code/interp_mips
: > in.txt; rm -f irix.delta console.log scsi.log
SCSIDBG=1 ./interp_mips \
  --file /home/dsheffie/code/chd-dumper/extracted/unix.clean \
  --prom /home/dsheffie/code/r9999/arcs/henry_arcs.bin \
  --start-pc 0xbfc00000 --disk /home/dsheffie/code/iris/irix65-clean.img \
  --disk-delta irix.delta \
  < in.txt > console.log 2> scsi.log &
# wait for "IRIS console login:" in console.log (~10-80s; clean image is fast)
printf 'root\n' >> in.txt        # then wait ~7s for "TERM = (vt100)"
printf '\n'     >> in.txt        # accept vt100 -> shell prompt "IRIS 1#"
# once "IRIS 1#" appears, kill the interp -> scsi.log = power-on..prompt
pkill -x interp_mips             # NB: kill by exact name; `pkill -f` disrupts the harness
```

Log line formats (in `scsi.log`):
- Decoded command:
  `sgi_scsi: CDB 28 00 .. .. .. .. 00 .. .. 00 dest=1 lun=0`
  (10 CDB bytes; `dest` = SCSI target, `lun` = logical unit)
- Absent target: `sgi_scsi: select target N -> no device (selection timeout)`
- Chunked DMA boundary: `sgi_scsi: data-in paused at 258048/262144 (chunked) -> INTRQ`
  (paused at `X` of `Y` bytes) and the matching `... resume from X/Y`

The log also contains noisy per-register `pio_w` lines — filter to `CDB` /
`select target` / `paused` for analysis.

**Caveats / scope of this capture**
- One boot of the clean 6.5.22 image, through login (`root`, no password) and
  the `tset` terminal handshake, to the `csh` prompt.
- Writes go to the COW `--disk-delta` overlay; the base image stays read-only.
- The clean image's one-time kernel reconfigure was already done, so this is a
  "warm" boot (no `lboot` rebuild traffic).

---

## 2. SCSI command profile (7,654 commands + 18 selection-timeouts)

| Count | Opcode | Command           | Share |
|------:|:------:|-------------------|------:|
| 5,486 | 0x28   | READ(10)          | 71.7% |
| 2,108 | 0x2a   | WRITE(10)         | 27.5% |
|    26 | 0x12   | INQUIRY           |       |
|    13 | 0x00   | TEST UNIT READY   |       |
|    12 | 0x25   | READ CAPACITY(10) |       |
|     7 | 0x03   | REQUEST SENSE     |       |
|     2 | 0x1a   | MODE SENSE(6)     |       |

~99.2% of commands are filesystem I/O (READ/WRITE(10)); ~60 are discovery/control.

### Target / LUN distribution
- **target 1, LUN 0** — the real disk — 7,640 commands.
- **target 1, LUNs 1–7** — 2 each (INQUIRY + REQUEST SENSE): IRIX's LUN scan.
  The model returns CHECK CONDITION (LUN-not-supported) for LUN≠0, so each stops
  after one probe.
- **targets 2–7** — 3 selection-timeouts each (18 total): the absent-target
  behaviour added in commit `9c052ec` (single disk at SCSI ID 1). Target 0 is
  not probed; target 7 = host-adapter ID.

### Discovery handshake (first commands, in order)
```
INQUIRY t1l0                       PROM probes the boot disk
INQUIRY+REQ-SENSE t1l1..t1l7       LUN scan (only LUN 0 real)
INQUIRY, TEST UNIT READY, MODE SENSE(6) t1l0
READ(10) lba=0 blocks=1            volume header
READ CAPACITY, INQUIRY, TEST UNIT READY ...
→ bulk READ(10)/WRITE(10) filesystem traffic
```

---

## 3. DMA transfer-size stats

Each READ/WRITE(10) is one logical SCSI DMA; size = `cdb[7:8]` blocks × 512.

### READ(10) — 5,486 transfers, 73.3 MB
| metric  | blocks | bytes  |
|---------|-------:|-------:|
| min     | 1      | 512 B  |
| median  | 8      | 4 KB   |
| mean    | 27.4   | ~14 KB |
| max     | 1024   | 256 KB |

Distribution: **56% = 4 KB (8 blk)**, 36% in 8–32 KB, ~0.2% > 128 KB.
Most common: `8blk×3078`, `64blk×1408` (32 KB), `16blk×271`, `1blk×133`.

### WRITE(10) — 2,108 transfers, 27.7 MB
| metric  | blocks | bytes  |
|---------|-------:|-------:|
| min     | 1      | 512 B  |
| median  | 8      | 4 KB   |
| mean    | 26.9   | ~14 KB |
| max     | 800    | 400 KB |

Distribution: **70% = 4 KB (8 blk)**, 17% in 8–32 KB, ~2% in 128–256 KB.
Most common: `8blk×1485`, `64blk×238` (32 KB), `512blk×27` (256 KB).

The dominant 4 KB size is the XFS filesystem block/page; 32 KB is XFS
readahead/log clustering.

### Chunked DMA (HPC3 descriptor-chain EOX split) — 49 events
Large SCSI transfers do not move in one DMA: IRIX caps a single HPC3 descriptor
chain and pause/resumes the rest (handled by `pause_transfer()` / the resume
path in `select_and_transfer()`).
- **Chunk granularity at pause: 252 KB (504 blocks = 0x3F000)** ×43, and
  504 KB (1008 blocks) ×6.
- Transfers that got chunked: 256 KB (512 blk)×28, 512 KB (1024 blk)×10,
  400 KB (800 blk)×7, plus a few odd sizes (1016/680/704 blk).

**Effective maximum single DMA ≈ 252 KB**; anything larger is split at
504-block boundaries.

Net character: small-transfer-dominated (median 4 KB; 90%+ ≤ 32 KB) with a long
tail of hundred-KB transfers that exercise the chunked-DMA path. READ:WRITE byte
ratio ≈ 73 MB : 28 MB — the writes are IRIX rc-script churn on `/var`, logs,
`/tmp`, and the XFS journal.

---

## 4. Regeneration / extension scripts

Opcode histogram (one-liner over a captured `scsi.log`):
```bash
grep -aoE "CDB [0-9a-f]{2}" scsi.log | awk '{print $2}' | sort | uniq -c | sort -rn
```

Full analysis (target/LUN, data totals, size distribution, chunking) — Python,
reads `scsi.log`:
```python
import re, statistics
from collections import Counter
rx = re.compile(r"CDB ([0-9a-f ]{29}) dest=(\d) lun=(\d)")
rd, wr, cmds, tmo = [], [], Counter(), Counter()
for line in open("scsi.log", "rb"):
    s = line.decode("latin1")
    m = rx.search(s)
    if m:
        b = [int(x, 16) for x in m.group(1).split()]
        cmds[(b[0], int(m.group(2)), int(m.group(3)))] += 1
        if b[0] in (0x28, 0x2a):
            blocks = (b[7] << 8 | b[8])
            (rd if b[0] == 0x28 else wr).append(blocks)
    t = re.search(r"select target (\d) -> no device", s)
    if t: tmo[int(t.group(1))] += 1
paused = [tuple(map(int, mm.groups()))
          for line in open("scsi.log", "rb")
          for mm in [re.search(r"paused at (\d+)/(\d+)", line.decode('latin1'))] if mm]
# rd/wr = per-transfer block counts; cmds = (opcode,target,lun) histogram;
# tmo = selection-timeouts per target; paused = [(chunk_bytes, full_bytes), ...]
```

Opcode names: 00 TEST-UNIT-READY, 03 REQUEST-SENSE, 12 INQUIRY, 15 MODE-SELECT(6),
1a MODE-SENSE(6), 1b START/STOP, 25 READ-CAPACITY, 28 READ(10), 2a WRITE(10).

### Ideas for a follow-up session
- Break the profile by **boot phase** (PROM vs kernel root-mount vs rc-scripts):
  the SCSI model has no timestamps, but you can correlate with `console.log`
  milestones, or add an `s->icnt` stamp to the CDB `SPRINTF`.
- Per-LBA heat map (`cdb[2:5]`) to see hot regions (inodes/superblock/dir blocks).
- Confirm the 252 KB chunk cap maps to the HPC3 descriptor count limit
  (`count[13:0]`, ≤8192 B/descriptor) vs. an XFS max-contiguous-I/O setting.
