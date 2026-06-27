# IRIX 6.5 post-login hang — debugging notes + gdb stub + proc-table walk

Status (2026-06-26): **SOLVED.** IRIX 6.5.22 `/unix` now boots on `interp_mips`
to a **fully interactive serial-console shell** (`IRIS 1# `; `echo`/`uname -a`
work). Previously it reached `IRIS console login:`, accepted `root` + the
`TERM = (vt100)` Enter, then hung before any shell prompt. This doc captures the
reproducible setup, the gdb-server stub used to root-cause it, the IRIX
kernel-structure offsets, and the fix.

## THE FIX (one line, `sgi_scc.cc`)
The Z8530 (`du`) console ISR reads **RR1** (Rx error/status) right before pulling
the data byte, and **drops the char if any error bit is set**. Our SCC model
returned the **RR0** value for any non-RR3 control read; RR0's `ALL_SENT` bit
(0x40) aliases **RR1's CRC/Framing-Error bit (D6)**, so *every* received console
char looked like a framing error and was discarded — the ISR read the byte but
never `allocb`/`putnext`'d it up the stream. Fix: handle RR1 explicitly,
reporting a clean receive (error bits clear):
```cpp
if (p == 1)  /* RR1: Rx error/status — report no parity/overrun/framing error */
    return tx_busy[ch] ? 0x00 : 0x01;   /* D0 All Sent; D4/D5/D6 clear */
```
Symptom chain it explains: `tset` (run from `csh`'s `.login`) does a STREAMS
`read` on the console; the typed char's ISR dropped it as a framing error, so
`tset` never woke; `csh` slept waiting on its child `tset`; no prompt.

---

(historical root-cause narrative below)

TL;DR root cause: login execs `csh`, whose `.login` runs **`tset`**. `tset`
blocks in a STREAMS `read` on the console stream head. Console input *does* fire
the SCC Rx interrupt and the `du` ISR runs — but the ISR reads **RR1**, sees a
(bogus) framing error, and drops the char before `allocb`/`putnext`, so `tset`
is never woken; `csh` sleeps waiting on its child `tset`. So no prompt. (The
earlier `getty`/`login` read of `root`+Enter worked because that path polled the
data register and didn't gate on RR1.)

---

## 1. Reproduce the boot to the hang

Files (all already present on this box):

| role   | path |
|--------|------|
| kernel | `/home/dsheffie/code/chd-dumper/extracted/unix.clean` (IRIX 6.5.22f, N32 MIPS-III BE ELF, **unstripped, full debug_info**) |
| PROM   | `/home/dsheffie/code/r9999/arcs/henry_arcs.bin` (flat blob @ 0x1fc00000; kentry slot defaults to /unix entry) |
| disk   | `/home/dsheffie/code/iris/irix65-clean.img` (clean 6.5.22 install, 20 GB raw) |

`./boot_irix.sh` wraps this. Manual form, driving the console from a **regular
file** (not a tty/fifo — a regular file never blocks and picks up appends after
EOF, which is exactly what we want):

```bash
cd /home/dsheffie/code/interp_mips
: > in.txt                       # console input sink
./interp_mips \
  --file /home/dsheffie/code/chd-dumper/extracted/unix.clean \
  --prom /home/dsheffie/code/r9999/arcs/henry_arcs.bin \
  --start-pc 0xbfc00000 \
  --disk /home/dsheffie/code/iris/irix65-clean.img \
  --disk-delta irix.delta \
  --gdb 1234 \                   # optional: open the gdb stub on :1234
  < in.txt > console.log 2> dbg.log &
```

- Reaches `IRIS console login:` in **~70 s** of wall time.
- Log into the hang:
  ```bash
  printf 'root\n' >> in.txt   # wait ~7s, "TERM = (vt100)" appears
  printf '\n'     >> in.txt   # accept vt100 -> hangs here
  ```
- `DEVTRACE=1` enables device tracing **on stderr** (sgi_hpc / sgi_mc); console
  stays clean on stdout. Other env knobs: `SCCRX` (SCC Rx debug), `INTSRC`
  (which CP0 IP bit gets delivered), `FASTDELAY=1` (collapse us_delay loops).

### Process / environment gotchas (cost me real time)
- **A timed-out shell kills its whole process group**, including `nohup`
  children. Launch the ISS as a background task that survives the launching
  shell; kill it by **explicit PID** (`kill -9 <pid>`).
- **`pkill -f interp_mips` disrupts this session** (returns exit 144). Always
  `kill` the specific PID from `ps -eo pid,cmd | grep 'interp_mips --file'`.
- Stock Ubuntu `gdb` has **no MIPS** support → need `gdb-multiarch`
  (`apt install gdb-multiarch`, not installed here) **or** use `rsp.py` below.

---

## 2. The gdb-server stub (`--gdb <port>`)

New files `gdbstub.{cc,hh}`, wired into the main loop (`main.cc`:
`s->gdb->step_hook(s)` right after `maybe_take_interrupt`). Boots **full speed
while unattached** (non-blocking `accept` polled every 64k insns); attaching a
client stops the target. **Uncommitted** as of this writing.

Capabilities (gdb RSP): `g`/`G` regs, `m`/`M` memory by **kernel VA**
(translated via `tlb_probe_ro`, so kseg0/mapped both work), `Z0`/`z0`
software breakpoints (kept in the stub — guest text is never patched), `c`/`s`,
and a `target.xml` over `qXfer` using the legacy MIPS regnum layout
(0-31 gpr, 32 status, 33 lo, 34 hi, 35 badvaddr, 36 cause, 37 pc, 38-69 fp,
70 fcsr, 71 fir; all 64-bit except fcsr/fir).

Robustness fixes made this session: stub ignores **SIGPIPE** (a client
disconnect used to kill the ISS with exit 141) and treats a short `write` as a
disconnect.

### Using it with real gdb
```
gdb-multiarch /home/dsheffie/code/chd-dumper/extracted/unix.clean
(gdb) set endian big
(gdb) set architecture mips:isa64
(gdb) target remote :1234
(gdb) info registers ; x/8i $pc ; break clock ; continue ...
```

### Using it with `rsp.py` (no toolchain needed)
`rsp.py` (in this repo) is a tiny RSP client. **It sets `TCP_NODELAY`** — without
that, each round-trip is ~50 ms and a memory scan times out; with it, ~0.03 ms.

```python
from rsp import RSP
r = RSP(port=1234); r.cmd("qSupported"); r.cmd("?")
rg = r.regs()                 # dict: r0..r31, status, lo, hi, badvaddr, cause, pc
r.rdmem(addr, n); r.rd32(addr) # addr = guest VA (sign-extend kseg, see sx() below)
r.setbp(a); r.delbp(a); r.cont(); r.step()
```
Connecting stops the target; closing the socket resumes it (consistent snapshot
for the duration of the session).

---

## 3. IRIX 6.5 IP22 kernel structure offsets (for `unix.clean`)

Symbols come straight from the unstripped kernel:
`mips-linux-gnu-objdump -t unix.clean`  (and `-d` for disasm).
**Gotcha:** objdump prints `addiu`/`lw` immediates in **decimal** — e.g. the low
half of `slpsvs` (0x88322630) is `0x2630` = **9776**; grep decimal, not hex.

```python
def sx(v):  # kseg pointers are stored as 32-bit; sign-extend to 64-bit VA
    return v | 0xffffffff00000000 if v & 0x80000000 else v
```

### "current process" walk
| what | where |
|------|-------|
| current kthread/uthread ptr | 32-bit @ VA `0xFFFFFFFFFFFFA014` (sign-extend) |
| proc | `*(kthread + 488)` |
| comm (argv[0], 32 bytes) | `proc + 1512` |
| pid | `proc + 420` |
| proc → kthread | `proc + 68` |
| kthread saved resume PC | `kthread + 84` (== `swtch+320` = 0x8816bb7c when asleep) |
| kthread BSD-sleep wait channel | `kthread + 156` |

### Enumerate ALL processes (the proc table is a vproc/pid maze; just scan)
Proc structs are 4 KB-aligned in the kernel heap. Scan kseg pages and accept a
page as a proc if `comm`@+1512 is printable and `0 < pid < 65536`@+420:
```python
for page in range(0x88000000, 0x8c000000, 0x1000):
    b = r.rdmem(sx(page)+1512, 16)
    n = b.split(b'\0')[0]
    if 2 <= len(n) <= 15 and all(32 <= c < 127 for c in n):
        pid = r.rd32(sx(page)+420)
        if pid and 0 < pid < 65536:
            print(pid, hex(page), n.decode())
```
A handful of garbage false-positives appear (random printable bytes at +1512);
the real procs have clean names. (`pidtab`@0x882f7940 / `nproc`@0x882f5e9c /
`procscan`@0x8816ac94 / `pid_to_vproc`@0x8816a930 exist but the pid hash is a
locked vproc structure — scanning is simpler and reliable.)

### BSD `sleep()` / `wakeup()` (this is how IRIX parks a blocked thread)
| symbol | addr | role |
|--------|------|------|
| `slpsvs` | 0x88322630 | 512-entry **hashed wait-channel table** (each entry a `sync variable`/sv) |
| `slpinit` | 0x8816b198 | fills the 512 entries via `init_sv` |
| `sleep` | 0x8816b0f0 | `bucket = hash(chan)`; `sv_wait_sig(&slpsvs[bucket])`; stashes the real `chan` at **kthread+156** for the duration |
| `wakeup` | 0x8816b154 | `sv_broadcast(&slpsvs[hash(chan)])` |
| `swtch` | 0x8816ba3c | context switch; a sleeping thread's saved PC is `swtch+320` |

`hash(chan)` = repeat `v = chan + (v>>9)` three times, then `& 0x1ff`.
**To find what a blocked thread waits on: read `kthread+156`.**

### STREAMS `queue_t` (the wait object turned out to be a stream head)
| offset | field | note |
|--------|-------|------|
| +0x00 | `q_qinfo` | → `strdata`(0x882e77b0, read side) / `stwdata`(0x882e77d0, write side) = the **stream-head** module |
| +0x14 | `q_ptr` | private (→ stdata) |
| +0x1c | `q_flag` | bits: `QENAB`=0x1, **`QWANTR`=0x2** (blocked reader), `QWANTW`=0x4, `QFULL`=0x8, **`QREADR`=0x10**, **`QUSE`=0x20** |

### Handy syscall-handler addresses (for breakpoints; resolve via objdump -t)
`syscall` dispatcher 0x880f5750 · read 0x8813d094 · write 0x8813d540 ·
open 0x8813c698 · close 0x8813cb3c · select 0x880a3ce8 · ioctl 0x88141ea8 ·
lseek 0x8813ed60 · sproc 0x8817a97c · sprocsp 0x8817a9cc · clock 0x880f8a3c ·
idle 0x8814e998 · flush_console 0x8814e964.
**Note:** the kernel's `sysent` table is @0x882e85b0 but its 16-byte entries are
fiddly to index by syscall number; resolve handlers by symbol instead. Also
**`select` is syscall #1076, NOT #1100** (1100 = `sprocsp`) — an earlier
`select_readfds @ num==1100` label in `interpret.cc` is wrong.

---

## 4. Root-cause of the post-login hang (reproducible walk)

At the `TERM = (vt100)` hang, with `--gdb 1234` and `rsp.py`:

1. **Who's running?** Break on the `syscall` dispatcher / individual handlers,
   read `comm` of the current proc at each hit. Only daemons appear
   (`sendmail`, `routed`, `nsd`, `syslogd`, `timed`, ...). The shell never
   makes a syscall → it's blocked in the kernel.
2. **Enumerate all procs** (scan above). The real foreground chain exists:
   `init(1)` → ... → **`csh`(pid 1123)** → **`tset`(pid 1260)**. Also `xdm`(1196)
   is up (graphical login on unmodeled gfx — separate concern).
3. **Both csh and tset are asleep**: `kthread+84 == swtch+320` for both.
4. **What do they wait on?** `kthread+156`:
   - `csh` → a heap object (its child `tset`'s exit/wait struct).
   - `tset` → **0x88da7480**, a STREAMS `queue_t` whose `q_qinfo` = `strdata`
     (stream head, read side), `q_flag` = **0x32 = QWANTR|QREADR|QUSE** =
     **a blocked reader on the console stream head**.
5. **Decisive test — input does NOT wake it.** Detach (resume), append a char to
   `in.txt`, wait, re-attach: tset's `kthread+156`, the queue's `q_flag`, and the
   saved PC are **all unchanged**. So a typed character never reaches /
   never wakes the post-login console reader.

Conclusion: the **SCC Rx interrupt → IRIX STREAMS console driver →
stream-head `wakeup`** path does not deliver typed characters to the
post-login (interrupt-driven, STREAMS) reader. Getty/login read fine earlier
(polled or differently armed), so basic Rx works; the wakeup for tset's
`strread` is the missing piece.

### How the fix was pinned (reproducible with the stub)
With `--gdb` + `rsp.py` at the hang:
1. Inject a char into `in.txt`; SCC trace (`SCCRX=1`) shows `rx push` then the
   ISR reads `RR3, RR0, RR1, data` — so the **interrupt fires and the ISR reads
   the byte**. So Rx-int → IP2 → ISR works.
2. Find the ISR: `du_init` (0x880cc9c8) does `setlclvector(21, 0x880ce3b4, 0)` →
   du ISR = **0x880ce3b4**. Its call graph includes `allocb`/`str_conmsg`/
   `putnext(jalr v0)` — the STREAMS delivery path.
3. Break on `du ISR`(0x880ce3b4) + `allocb`(0x880c2630) + `str_conmsg` +
   `wakeup`, add a `clock`(0x880f8a3c) bp so `cont()` always returns, inject ONE
   char: **the du ISR fires but `allocb`/`putnext`/`wakeup` are NEVER reached**
   → the ISR drops the char. The only thing it reads between RR0 and `data` is
   **RR1**, which our model mis-returned as RR0 (framing-error alias). Fixed.

---

## 5. Files touched this session (all UNCOMMITTED)
- **New:** `gdbstub.cc`, `gdbstub.hh`, `rsp.py`, this doc.
- **Wiring:** `Makefile` (+gdbstub.o), `main.cc` (`--gdb`, loop hook),
  `interpret.hh`/`interpret.cc` (`state_t::gdb`, `gdb_mem_read/write`).
- **Debug aids (optional to keep):** `sgi_hpc.cc`/`sgi_mc.cc` DPRINTF→stderr so
  `DEVTRACE` doesn't pollute the console; a Seeq enet "no-carrier" stub
  @0x54000; an MC-access PC trace; an `INTSRC` interrupt-source probe; a
  user-syscall ring + `comm` in `dump_current_process` (SIGUSR1).
