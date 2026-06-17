# IRIX /unix boot in interp_mips — status & findings

Goal: boot the IRIX 6.5.22 N32 `/unix` kernel in the standalone fast ISS, get
console text, map the boot, and report what the r9999 RTL must implement.

Run:
```
./interp_mips --file /home/dsheffie/code/chd-dumper/extracted/unix \
              --arcs /home/dsheffie/code/r9999/arcs/arcs_irix.bin \
              --maxicnt N
```
Kernel entry `start` = 0x88005960. Entry regs (a0=argc, a1=argv, a2=envp) are now
synthesized by the **pseudo-BIOS** (`pseudo_bios.cc`, `install_pseudo_bios`) — the
old a0=8,a1=0,a2=0 made `getargs` derail. See the 2026-06-15 update section below.
Console = Z8530 SCC at phys 0x1fbd9830 (sgi_scc.{hh,cc}, wired in sparse_mem.cc).

Useful env knobs (main.cc / interpret.cc):
- `PCTRACE=N`   sample pc/ra/sp every N insns to stderr
- `PCTRACEOUT=f`+`PCTRACE` one pc per line to file f (delay slots included)
- `FINEWIN=lo:hi` per-insn pc trace for icnt window [lo,hi)
- `CALLWIN=lo:hi` log every jal/jalr (caller->callee) in the icnt window
- `TLBDBG=1`    print every TLB exception (va, code, refill/xtlb, ctx) + TLB dump on refill
- `KPTEDBG=1`   dump faultPC/EPC/EXL/Context + full TLB on the first faults at 0xff800000
- `KMISSDBG=1`  at kmiss entry (0x880162c8), dump the PDA/kptbl PT-root walk for a
                KPTEBASE-region fault; also logs `resume` (thread-switch) executions
- `ROMVEC=1`    log every control transfer into the ARCS romvec stub blob (PA 0x1000-0x1fff)
- `A1PROBE`/`KPTEDBG`/`CALLWIN` — see interpret.cc for exact formats
- `DEVTRACE=1`  re-enable MC/HPC device register spew

## What was added this session (the real translating TLB)

interp_mips previously used a 1:1 `va2pa` (va & 0x1fffffff) — no translation. That
masked many near-null / mapped-pointer bugs and let both Linux and IRIX limp far on
self-consistent-but-wrong physical addresses. The brief requires a **real R4000 TLB**.

Added (interpret.cc):
- `va_translate(s, va, op)` — segment decode + 48-entry TLB lookup. kseg0/kseg1
  stay unmapped fast-path (PA = va&0x1fffffff); xkphys direct; useg/kuseg/kseg2/
  kseg3 + 64-bit xkuseg/xksseg/xkseg go through the TLB. Sets `s->tlb_fault` on a
  miss/invalid/modified; every load/store/fetch site checks it and aborts.
- `raise_tlb()` / `tlb_set_fault_state()` — on a TLB exception set BadVAddr,
  EntryHi.VPN2 (ASID preserved), **Context** (BadVPN2=VA[31:13]<<4) and **XContext**
  (BadVPN2=VA[39:13]<<4, R field), so the kernel's refill handler can index the
  self-mapped page table. Without Context the handler reads garbage.
- BEV-aware exception vectoring (`exc_vector`): BEV=0 -> base 0x80000000 (refill
  0x000 / XTLB 0x080 only when EXL==0, else general 0x180); BEV=1 -> 0xBFC00200.
  Previously ALL exceptions hardcoded 0xBFC00180 (wrong; only worked because the
  1:1 boot never actually took an exception on the happy path).
- All load/store/fetch paths route through `va_translate` and bail on `tlb_fault`.

Two subtle TLB-match bugs found & fixed during bring-up:
1. **R-field / 32-bit addressing.** The kernel writes EntryHi for kseg2/kseg3 via
   32-bit MTC0 (R=0, value zero-extended). A faulting 32-bit kseg3 VA sign-extends
   to 0xFFFFFFFFxxxxxxxx (R=3). Comparing the R field then never matched. Fix: in
   32-bit addressing (hi32==0 or 0xffffffff) compare only VPN2[31:13] and skip R;
   compare the full VPN2[39:13]+R only in 64-bit addressing.
2. **even/odd page select bit.** A TLB entry maps an even/odd PAIR. For 4 KB pages
   the in-page offset is 12 bits (0xfff) and the even/odd select is bit 12 (0x1000)
   — NOT 13 bits / bit 13. Original code used bit 13 and picked the wrong half
   (odd, V=0) -> spurious TLB-Invalid. Fix: pair_mask=pm|0x1fff, off_mask=pair>>1,
   sel_bit=(pair+1)>>1.

Also injected an `eaddr` env var (main.cc): the r9999-irix arcs blob's
`stub_getenv` returns NULL; with the real TLB, `init_sysid` then does
`etoh(getenv("eaddr"))` = `etoh(NULL)` = `lbu 0(0)` -> TLB miss on VA 0 -> derail.
Real PROM firmware returns the EEPROM eaddr. We patch the blob's `stub_getenv`
(blob offset 0xe68) to return a pointer to an injected "08:00:69:12:34:56" string
(blob offset 0xf00 -> kseg1 0xa0001f00). Guarded on blob size >= 0xe78 (the IRIX
blob is 0xf00 bytes; stub_getenv is at 0xe68) so it does NOT touch the much smaller
752-byte Linux arcs_fw blob. (The arcs_irix.S source is the
proper home for this, but it is read-only to this session.)

## Boot progress (real TLB)

CONSOLE TEXT CONFIRMED via the SCC: the IRIX kernel's own panic handler prints
through the Z8530 model. With the real TLB the kernel exercises its true VM
bring-up and faults early (icnt ~80,000) during `mlsetup`:

```
PANIC: TLBMISS: KERNEL FAULT
PC: 0x0 ep: 0x88654cc8
EXC code:8, `Read TLB Miss '
Bad addr: 0xff800000, cause: 0x8<...>
[Press reset to restart the machine.]
```

Boot path to the panic (verified by nm/objdump + TLBDBG):
- `start`(0x88005960) -> ... -> `mlsetup`(0x8814a0d0)
- `mlsetup` -> `wirepda`(0x881689b0) -> `tlbwired`: wires the per-CPU PDA into the
  TLB (tlb[0]: EntryHi 0xffffa000 -> PFN 0x8393). The `jr ra` delay slot
  `sw at,-24000(zero)` stores to **0xffffa240**, resolved by the wired PDA entry.
  THIS is exactly the r9999 RTL's old wall (return-from-wirepda); the real TLB
  here resolves it correctly (was the even/odd + R-field bugs above).
- `mlsetup` -> `bzero`(0x8801a83c) zeroing a kvalloc'd region at **0xc0000000**
  (`sdl zero,0(a0)` @ 0x8801a860) -> TLB-store refill.
- The refill handler (`utlbmiss_prolog_up` @ 0x88016680, copied to 0x80000000)
  does `mfc0 k0,Context; sra k0,k0,1; lw k1,0(k0)` — walks the **self-mapped
  linear page table**: VA 0xc0000000 -> PTE at 0xffb00000 -> PTE-of-PTE -> ... and
  the nested walk reaches **0xff800000** (KPTEBASE, the root of the self-mapped
  page table) which has NO valid mapping yet -> IRIX panics "TLBMISS KERNEL FAULT".

## Update 2026-06-15 (later) — KPTEBASE wall SOLVED: EPC-on-nested-exception bug; IRIX now banners + reaches root-device mount

**Root cause was a CPU exception-semantics bug in interp_mips, NOT a pmap/backing
issue.** `set_exc_pc` (interpret.cc) wrote EPC + Cause.BD unconditionally; on R4000
they must update ONLY when `Status.EXL==0`. The KPTEBASE self-map refill relies on
this: a nested TLB miss inside the 0x80000000 refill handler must leave EPC = the
ORIGINAL access (0x8801a860), so its eret retries the whole refill once `kmissnxt`
maps the PT page. We were updating EPC to 0x80000008 (mid-handler) → eret resumed
with k0 clobbered by kmissnxt → read addr 0 → recursed to 0xff800000 → panic.

Fix: `set_exc_pc` returns early if `SR_EXL` already set. (kvalloc/kptbl were fine
all along — `kptbl[c0000000]` was a valid `0x4021009f` at the fault; MAME round-6's
"kvalloc leaves it 0x1" premise didn't match our actual state.)

**Result:** boot blows past KPTEBASE; prints the release banner
("IRIX Release 6.5 IP22 Version 10070055 System V ...") and runs to **~13.07M insns**,
halting cleanly (`brk=1`) at **root-filesystem mount**: "Root device
/hw/scsi_ctlr/0/.../partition/0/block not available" + "pbus configuration failed
for channel 0-3". New wall = no SCSI disk / PBUS device model (storage, not VM/CPU).
Directed except/tlb/cp0/unaligned tests still green (fix only affects nested EXL=1).

Debug knobs added this hunt (env-gated, kept): KMISSDBG, KVALDBG, KMISSTRACE +
`tlb_probe_ro` (non-faulting translate for debug reads).

## Update 2026-06-15 — pseudo-BIOS + R4600 path; wall = linear self-map not backed

Boot now runs to **~6.4M instructions** (was ~80k) on the **R4600** path. Three fixes
got us here (all measured against MAME co-sim, `r9999/MAME_QUESTIONS.md` Q5 thread):
1. **pseudo-BIOS** (`pseudo_bios.cc`): synthesizes sash's real argv/envp handoff
   (a0=argc, a1=argv, a2=envp as kseg0 ptrs near top of the 16 MiB bank). Fixed
   `getargs` derail.
2. **Config = 0x0002e4b3** (`interpret.cc` initState): R4600 cache geometry so
   `mlreset` derives `cachecolormask=1`; the old 0x00088200 made `pagecoloralign`
   spin forever.
3. **IOC2 SYSID 0x26** (`sgi_hpc.cc`, phys 0x1fbd9858): `getsysid` picks the
   guinness/Indy board id.

**Current panic** (R4600 path reaches it earlier than the old R4000 path):
```
PANIC: TLBMISS: KERNEL FAULT   PC: 0x0  ep: 0x883facc8
EXC code:8 'Read TLB Miss'   Bad addr: 0xff800000
```

**The fault chain (TLBDBG):**
```
8801a860  store 0xc0000000   refill exl=0 ctx=ff600000   [bzero of a kvalloc'd region]
80000008  load  0xffb00000   refill exl=1               [linear PTE for 0xc0000000]
80000008  load  0x00000000   refill exl=0
80000008  load  0xff800000   refill exl=1 -> general -> kmiss -> PANIC
```
The refill handler at 0x80000000 (`mfc0 k0,Context; sra k0,k0,1; lw k1,0(k0)`,
faultPC +8) walks the self-mapped linear page table. Its arithmetic and the
Context/PTEBase handling are **correct** (PTEBase = 0xff000000, preserved) — this is
NOT an instruction bug.

**Root cause (KMISSDBG probe):** the kernel page-table root **exists** —
`kptbl` (0x8832cf58) = **0x88392000** (PA 0x08392000, exactly the arena PA MAME
predicted), `klim`=0x2400 — so `mlsetup`'s arena allocation ran. BUT the **linear
self-map PTE pages that root should contain are not backed**: the walk for
0xc0000000 faults on its PTE addr 0xffb00000, and the PT-of-PT walk faults on
0xff800000, neither resolving. kmiss can't rescue 0xff800000 because that VA is in
the per-process pmap window (PDA+0x378) and `resume` (thread-switch) has never run
(PDA ptr at 0xffffa014 = 0, `[resume]` never logged) — legitimately, this early.

So: **kptbl exists but its linear-self-map PTE pages are unpopulated.** Next
co-sim round (Q5 round-6): ask MAME, at the 0xc0000000 bzero fault, what
`kptbl[idx(0xffb00000)]` / the linear PTE for 0xc0000000 contains — i.e. which
`mlsetup`/`maputokptbl` step backs the linear-map PTE pages that ours leaves
invalid (round-5 noted the arena is init'd to PTE=0x1 global/invalid).

**Latent interp bug (NOT this path, flagged):** `mtc0` zero-extends to the 64-bit
shadow (`interpret.cc:2523`, `cpr0_64[rd]=(uint64_t)(uint32_t)gpr[rt]`) instead of
sign-extending. Harmless for the refill walk (it reads Context via 32-bit `mfc0`),
but wrong for any 64-bit (`dmfc0`) read of Context/XContext/EntryHi PTEBase.

## NEXT BLOCKER (well-defined)

IRIX's **self-mapped kernel page-table root** is not established before the first
mapped kvaddr access (`bzero(0xc0000000)`). The refill handler's linear-page-table
walk needs the page-table root page (covering 0xff800000 / KPTEBASE) to be either
**wired into the TLB** or pre-populated before `mlsetup` touches 0xc0000000. This
is IRIX `kvmsetup`/`tlbwired`-for-the-page-table-root territory and needs a
multi-iteration dig into IRIX VM init (kptbl @ 0x8832cf58; `maputokptbl`
@ 0x88146b18; `utlbmiss*` family @ 0x88016700+). The TLB datapath/exceptions
themselves are now correct (the PDA wired entry resolves; Context indexes the
right PTE — verified Context=0xff600000 for VA 0xc0000000, i.e. PTEBase 0xff000000
| BadVPN2 0x600000).

## What the r9999 RTL must implement for IRIX (feedback)

1. **Real translating TLB** with R4000 semantics — exactly what the RTL is stuck
   on (the wirepda return). Key correctness points proven here:
   - 32-bit (KX=0) addressing: compare VPN2[31:13] only, ignore the R field
     (kernel writes EntryHi via 32-bit MTC0 with R=0; faulting kseg3 VAs are
     sign-extended with R=3). Comparing R in 32-bit mode is THE bug that makes the
     wired PDA entry never match the wirepda-return store to 0xffffa240.
   - even/odd page-pair select = bit 12 for 4 KB pages (offset is 12 bits), not
     bit 13.
   - On a TLB exception set BadVAddr, EntryHi.VPN2, **Context AND XContext** (fold
     the faulting VPN); the IRIX refill handler reads Context, `sra 1`, and loads
     the PTE — wrong Context => wrong PTE => derail.
2. **BEV-aware vectoring**: refill/XTLB to 0x80000000/0x80000080 only when EXL==0;
   everything else (incl. nested refill with EXL=1, Invalid, Modified) to
   0x80000180; BEV=1 base 0xBFC00200. IRIX relies on the dedicated refill vector
   for its fast linear-page-table walk.
3. **Delay-slot fault fidelity**: a TLB fault in a branch/jr delay slot must set
   EPC=branch pc, Cause.BD=1, and NOT take the branch (already handled here via
   run_delay_slot + tlb_fault). The wirepda return is precisely this case
   (`jr ra` whose delay slot store faults — though here it resolves, in the RTL it
   must vector correctly if unmapped).
4. **ARCS `GetEnvironmentVariable("eaddr")`** must return a real MAC string, else
   `init_sysid`->`etoh(NULL)` derails once the TLB is real (the 1:1 shortcut hid
   this). Belongs in arcs_irix.S.
5. **Self-mapped page-table root / kvmsetup**: the next wall for BOTH the ISS and
   the RTL — IRIX's KPTEBASE (0xff800000) self-map must be set up before the first
   mapped kvaddr access in mlsetup. (Open.)

## Regression note (Linux 1:1 boot)

The real TLB regresses the prior IP22 *Linux* (`vmlinux.32`) boot: `prom_init_cmdline`
reads a bogus argv pointer (s3=4 -> `lw 0(4)`) that the 1:1 va2pa masked (read PA 4
= zeros); the real TLB correctly faults on VA 4 -> spiral. This is a firmware/ABI
mismatch in the Linux arcs path that 1:1 hid, not a TLB bug. Linux boot under the
real TLB would need a correct ARCS argv/cmdline setup. Out of scope for the IRIX
mission; flagged here so it isn't mistaken for a TLB defect.
