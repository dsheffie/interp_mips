#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/utsname.h>


#include "interpret.hh"
#include "sgi_hpc.hh"
#include "sgi_scc.hh"
#include "disassemble.hh"
#include "helper.hh"
#include "globals.hh"
#include "inst_record.hh"

static fpMode currFpMode = fpMode::mips3;  /* IRIX N32 = MIPS-III, FR=1 flat 64-bit FP regs (odd regs legal, no fd+1 pairing) */

state_t::~state_t() {
  //std::cout << mem.bytes_allocated() << " bytes present in memory image\n";
  //delete &mem; // mem owned by caller (main.cc deletes the sparse_mem)
}

static void execCoproc0(uint32_t inst, state_t *s);
static void execCoproc2(uint32_t inst, state_t *s);

template <bool EL> void execMips(state_t *s);

void execMips(state_t *s) {
  execMips<IS_LITTLE_ENDIAN>(s);
}

uint64_t sext64(uint32_t x) {
  int64_t xx = static_cast<int64_t>(x);
  return (xx << 32) >> 32;
}

#if 1
std::ostream &operator<<(std::ostream &out, const state_t & s) {
  using namespace std;
  for(int i = 0; i < 32; i++) {
    out << getGPRName(i) << " : 0x"
	<< hex << s.gpr[i] << dec
	<< "(" << s.gpr[i] << ")\n";
  }
#if 0
  for(int i = 0; i < 32; i++) {
    out << "cpr0_" << i << " : 0x"
	<< hex << s.cpr0[i] << dec
	<< "\n";
  }
  for(int i = 0; i < 32; i++) {
    out << "cpr1_" << i << " : 0x"
	<< hex << s.cpr1[i] << dec
	<< "\n";
  }
  for(int i = 0; i < 5; i++) {
    out << "fcr" << i << " : 0x"
	<< hex << s.fcr1[i] << dec
	<< "\n";
  }
#endif
  out << "icnt : " << s.icnt << "\n";
  return out;
}
#endif

static uint32_t getConditionCode(state_t *s, uint32_t cc);
static void setConditionCode(state_t *s, uint32_t v, uint32_t cc);


/* IType instructions */
static void _lb(uint32_t inst, state_t *s);
static void _lbu(uint32_t inst, state_t *s);
static void _sb(uint32_t inst, state_t *s);


static void _mtc1(uint32_t inst, state_t *s);
static void _mfc1(uint32_t inst, state_t *s);

static void _sc(uint32_t inst, state_t *s);

/* FLOATING-POINT */
static void _c(uint32_t inst, state_t *s);

static void _cvts(uint32_t inst, state_t *s);
static void _cvtd(uint32_t inst, state_t *s);

static void _truncw(uint32_t inst, state_t *s);
static void _truncl(uint32_t inst, state_t *s);

static void _movci(uint32_t inst, state_t *s);

static void _fmovc(uint32_t inst, state_t *s);
static void _fmovn(uint32_t inst, state_t *s);
static void _fmovz(uint32_t inst, state_t *s);


static void _movcs(uint32_t inst, state_t *s);
static void _movcd(uint32_t inst, state_t *s);

static void _movnd(uint32_t inst, state_t *s);
static void _movns(uint32_t inst, state_t *s);
static void _movzd(uint32_t inst, state_t *s);
static void _movzs(uint32_t inst, state_t *s);

void initState(state_t *s) {
  /* Matches the RTL's cpr0_status_reg reset in exec.sv. */
  s->cpr0[CPR0_SR] |= SR_ERL | SR_BEV | SR_CU0 | SR_CU1 | SR_CU2;
  /* Random starts at max TLB index; it cycles downward to Wired */
  s->cpr0[CPR0_RANDOM] = state_t::NUM_TLB_ENTRIES - 1;
  /* PRId: read-only processor id (R4000 family for now) */
  s->cpr0[CPR0_PRID] = PRID_VALUE;
  s->cpr0_64[CPR0_PRID] = PRID_VALUE;
  /* Config: R4600 cache geometry (16 KB I$ + 16 KB D$, 32-byte lines, K0=3),
   * matching the real SGI Indy / MAME. mlreset derives cachecolormask from this;
   * the r9999 RTL's 0x00088200 gives the wrong mask and pagecoloralign loops
   * forever (MAME co-sim, MAME_QUESTIONS.md Q5 round-2). */
  s->cpr0[CPR0_CONFIG] = 0x0002e4b3;
  s->cpr0_64[CPR0_CONFIG] = 0x0002e4b3;
}

/* Raise MIPS Reserved Instruction exception (ExcCode=10).
 * Called by the interpreter when it encounters an unimplemented opcode.
 * Sets EPC/Cause/Status and redirects the interpreter to the exception
 * vector so execution follows the bare-metal exc_handler path. */
/* Sign-extend a 32-bit value to 64-bit, matching MIPS hardware behaviour
 * for registers and PC values where bit 31 indicates kernel address space. */
static inline state_t::reg_t sext32(uint32_t v) {
  return (state_t::reg_t)(int64_t)(int32_t)v;
}

/* Set EPC + Cause.BD for an exception, accounting for the branch-delay-slot case
 * (EPC = branch pc = pc-4, BD=1); matches the RTL.  Call before the ExcCode write
 * (which preserves BD by masking only bits [6:2]). */
static inline void set_exc_pc(state_t *s) {
  /* R4000: EPC (and Cause.BD) are updated ONLY if Status.EXL==0 at the time of the
   * exception. On a nested exception (EXL already set) they are preserved — so the
   * eventual eret returns to the ORIGINAL faulting instruction, not the nested
   * handler. (This is load-bearing for IRIX's self-mapped page-table refill: a
   * nested TLB miss in the 0x80000000 refill handler must leave EPC = the original
   * access, so retrying it re-runs the whole refill once the PT page is mapped.) */
  if(s->cpr0[CPR0_SR] & SR_EXL)
    return;
  /* EPC is a 64-bit register on R4000; keep the 32-bit (cpr0) and 64-bit
   * (cpr0_64) views consistent.  Linux's exception entry saves EPC via
   * `dmfc0 c0_epc` (reads cpr0_64) and restore_partial reloads it via
   * `dmtc0`, then erets -- so leaving cpr0_64[EPC] stale made eret return to
   * 0.  (IRIX read EPC via mfc0/eret, the 32-bit view, so it masked this.) */
  if(s->in_delay_slot) {
    s->cpr0[CPR0_EPC]    = (uint32_t)(s->pc - 4);
    s->cpr0_64[CPR0_EPC] = (uint64_t)(s->pc - 4);
    s->cpr0[CPR0_CAUSE] |=  (1u << 31);
  } else {
    s->cpr0[CPR0_EPC]    = (uint32_t)s->pc;
    s->cpr0_64[CPR0_EPC] = (uint64_t)s->pc;
    s->cpr0[CPR0_CAUSE] &= ~(1u << 31);
  }
}

/* R4000 exception vector selection.
 *   BEV=1 (bootstrap): base 0xBFC00200 (uncached ROM); refill 0x000, XTLB 0x080,
 *          general/all-others 0x180  ->  refill 0xBFC00200, general 0xBFC00380.
 *   BEV=0 (kernel installed handlers): base 0x80000000; TLB refill 0x000,
 *          XTLB 0x080, general/all-others 0x180.
 * The dedicated TLB-refill (TLBL/TLBS) vector at offset 0x000 (or XTLB 0x080) is
 * used ONLY when the exception is a TLB Refill (no matching entry) AND EXL==0 at
 * the time of the fault; otherwise (EXL already set, or any non-refill cause) the
 * general vector at 0x180 is used.  is_xtlb selects the 64-bit-addressing XTLB
 * refill slot. */
static inline uint32_t exc_vector(state_t *s, bool is_refill, bool exl_was_set,
                                  bool is_xtlb) {
  uint32_t base   = (s->cpr0[CPR0_SR] & SR_BEV) ? 0xBFC00200u : 0x80000000u;
  uint32_t offset = 0x180u;
  if(is_refill && !exl_was_set)
    offset = is_xtlb ? 0x080u : 0x000u;
  return base + offset;
}

/* Common tail for a synchronous exception: write Cause.ExcCode, set EXL (clearing
 * ERL), and vector.  set_exc_pc() (EPC + Cause.BD) must already have run. */
static inline void raise_common(state_t *s, uint32_t exccode) {
  bool exl_was_set = (s->cpr0[CPR0_SR] & SR_EXL) != 0;
  s->cpr0[CPR0_CAUSE] = (s->cpr0[CPR0_CAUSE] & ~(0x1fu << 2)) | (exccode << 2);
  s->cpr0[CPR0_SR]    = (s->cpr0[CPR0_SR] & ~SR_ERL) | SR_EXL;
  s->pc = sext32(exc_vector(s, /*is_refill=*/false, exl_was_set, /*xtlb=*/false));
}

/* CP0 Count/Compare timer + interrupt delivery (R4000 internal timer, the IP22
 * Linux clocksource/clockevent).  Called once per instruction step from the main
 * loop.  Count advances; Count==Compare sets Cause.IP[7] (timer pending).  An Int
 * exception (ExcCode 0) is taken at this instruction boundary when an enabled,
 * unmasked interrupt is pending: IE=1, EXL=0, ERL=0, (Cause.IP & Status.IM) != 0.
 * The kernel's timer ISR re-arms by writing Compare (which clears IP[7]; see the
 * MTC0 handler). */
/* How often to run the (relatively expensive) IOC2/INT2 device-interrupt poll +
 * delivery. Must be a power of two. IP2's sources are level-sensitive (stay
 * asserted until serviced), so polling every INT_POLL instructions only adds at
 * most that many instructions of delivery latency -- negligible for a functional
 * ISS -- while keeping ioc2_ip2_pending() (ioc2_local0_live + int_pending, ~10%
 * of run time when recomputed every instruction) off the hot path. NB: the SCC
 * TX drain (scc->tick) must stay exact every instruction -- the early PROM
 * banner uses polled serial whose RR0 wait depends on the per-instruction drain
 * timing, so throttling tick() hangs the boot. TODO: once every device is
 * modeled, replace this poll with a device-sets-a-flag scheme (the "right way"). */
static const uint64_t INT_POLL = 64;

void maybe_take_interrupt(state_t *s) {
  if(s->scc) s->scc->tick(1);                          /* serial TX timing: keep exact */
  /* CP0 Count is architecturally visible and the kernel's delay/clock
   * calibration reads it, so advance it (and latch the timer IP7) every
   * instruction -- cheap, and keeps the timer cycle-accurate. */
  s->cpr0[CPR0_COUNT] = (s->cpr0[CPR0_COUNT] + 1u) & 0xffffffffu;
  if(s->cpr0[CPR0_COUNT] == s->cpr0[CPR0_COMPARE])
    s->cpr0[CPR0_CAUSE] |= (1u << 15);                 /* IP[7] = timer */

  /* Poll devices every INT_POLL instructions, OR immediately when a device just
   * raised an interrupt (irq_poke). The SCSI completion is synchronous and the
   * kernel needs IP2 delivered right after the command -- a throttled delay loses
   * it and the wd93 driver hits its 60s timeout + bus reset. */
  if(!s->irq_poke && (s->icnt & (INT_POLL - 1)) != 0) return;
  s->irq_poke = false;

  /* IOC2/INT2 local0 (WD33C93 SCSI etc.) -> CP0 Cause IP[2] (bit 10). Level-
   * sensitive: set while an unmasked local0 source is asserted, else clear. */
  if(s->hpc && s->hpc->ioc2_ip2_pending()) s->cpr0[CPR0_CAUSE] |=  (1u << 10);
  else                                     s->cpr0[CPR0_CAUSE] &= ~(1u << 10);
  uint32_t sr = s->cpr0[CPR0_SR], cause = s->cpr0[CPR0_CAUSE];
  if((sr & SR_IE) && !(sr & (SR_EXL | SR_ERL)) && (((cause & sr) & 0xff00u) != 0u)) {
    set_exc_pc(s);
    raise_common(s, 0u);                               /* ExcCode 0 = Int */
  }
}

static void raise_adel(state_t *s) {
  set_exc_pc(s);
  raise_common(s, 4u);
}

static void raise_ades(state_t *s) {
  set_exc_pc(s);
  raise_common(s, 5u);
}

/* Reserved Instruction exception setup (ExcCode=10), no diagnostic message. */
static void take_exception_ri(state_t *s) {
  set_exc_pc(s);
  raise_common(s, 10u);
}

static void raise_ri(state_t *s, uint32_t inst) {
  fprintf(stderr, "unimplemented: opcode=0x%02x funct=0x%02x @ pc=0x%08x\n",
          inst >> 26, inst & 0x3fu, (uint32_t)s->pc);
  take_exception_ri(s);
}

/* Execute a branch/jump delay-slot instruction.  Returns true iff the delay slot
 * raised an exception (Status.EXL went 0->1, i.e. it vectored).  In that case the
 * branch/jump must NOT be taken: the exception has already redirected pc to the
 * handler with EPC = the branch pc (BD=1), and overwriting pc with the branch
 * target would SWALLOW the delay-slot fault (the RTL takes it -> co-sim diverges). */
template <bool EL>
static inline bool run_delay_slot(state_t *s) {
  bool exl_before = (s->cpr0[CPR0_SR] & SR_EXL) != 0;
  bool saved = s->in_delay_slot;
  s->in_delay_slot = true;
  execMips<EL>(s);
  s->in_delay_slot = saved;
  return (!exl_before) && ((s->cpr0[CPR0_SR] & SR_EXL) != 0);
}

/* 64-bit operating mode, matching exec.sv:2640-2646:
 *   kernel=(KSU==0)|EXL|ERL ; user=(KSU==2)&!EXL&!ERL ; super=(KSU==1)&!EXL&!ERL
 *   in_64b = (kernel&KX) | (user&UX) | (super&SX). */
static inline bool in_64b_mode(state_t *s) {
  uint32_t sr = s->cpr0[CPR0_SR];
  uint32_t ksu = (sr >> 3) & 3u;
  bool exl = (sr & SR_EXL) != 0, erl = (sr & SR_ERL) != 0;
  bool kernel = (ksu == 0u) || exl || erl;
  bool user   = (ksu == 2u) && !exl && !erl;
  bool super  = (ksu == 1u) && !exl && !erl;
  /* 64-bit operations are always valid in Kernel mode (KX gates 64-bit
   * addressing / the XTLB vector, not op availability); Supervisor/User need
   * SX/UX.  Must match decode_mips.sv's w_in_64b_mode for the co-sim. */
  return  kernel ||
         (user   && (sr & SR_UX)) ||
         (super  && (sr & SR_SX));
}

/* The 64-bit instructions decode_mips.sv gates behind 64-bit mode -- must match
 * EXACTLY so the co-sim agrees (RTL does NOT gate dsll32/dsrl32/dsra32, daddi,
 * or 64-bit loads/stores, so neither do we). */
static inline bool is_64b_gated(uint32_t inst) {
  uint32_t op = inst >> 26;
  if(op == 0x19) return true;                  /* daddiu */
  if(op != 0) return false;
  switch(inst & 0x3fu) {
    case 0x14: case 0x16: case 0x17:            /* dsllv dsrlv dsrav        */
    case 0x1c: case 0x1d: case 0x1e: case 0x1f: /* dmult dmultu ddiv ddivu  */
    case 0x2c: case 0x2d: case 0x2e: case 0x2f: /* dadd daddu dsub dsubu    */
    case 0x38: case 0x3a: case 0x3b:            /* dsll dsrl dsra           */
      return true;
    default: return false;
  }
}

static void raise_overflow(state_t *s) {
  set_exc_pc(s);
  raise_common(s, 12u);
}

static void raise_trap(state_t *s) {
  set_exc_pc(s);
  raise_common(s, 13u);
}

/* SYSCALL (ExcCode 8) / BREAK (ExcCode 9): trap into the kernel's general
 * exception handler (0x80000180). EPC = the syscall/break pc (the kernel's
 * handler advances past it before eret); set_exc_pc handles the delay-slot
 * case. (interp_mips was originally a user-mode sim that halted here; in
 * full-system OS mode these must vector to the kernel.) */
static bool tlb_probe_ro(state_t *s, uint64_t va, uint32_t *pa);
/* read a 32-bit big-endian word from a guest VA, non-faulting (returns false if
 * the VA is unmapped). Used to walk IRIX's curproc pointer chain. */
static bool guest_rd32_be(state_t *s, uint64_t va, uint32_t *out) {
  uint32_t pa;
  if(!tlb_probe_ro(s, va, &pa)) return false;
  *out = __builtin_bswap32(s->mem.get<uint32_t>(pa));   /* guest is big-endian */
  return true;
}
/* One-shot dump of the IRIX "current process" -- comm name + pid + the pc/mode
 * it is executing. Wired to SIGUSR1 in main() so the live emulator can be asked
 * "what is IRIX running right now?" from another shell (kill -USR1 <pid>). */
void dump_current_process(state_t *s) {
  uint32_t ut, proc;
  if(!guest_rd32_be(s, 0xFFFFFFFFFFFFA014ULL, &ut) || ut == 0) {
    fprintf(stderr, "[curproc] icnt=%lu pc=%08x: no current uthread\n",
            (unsigned long)s->icnt, (uint32_t)s->pc);
    return;
  }
  if(!guest_rd32_be(s, (uint64_t)(int64_t)(int32_t)ut + 488, &proc) || proc == 0) {
    fprintf(stderr, "[curproc] icnt=%lu pc=%08x: ut=%08x has no proc\n",
            (unsigned long)s->icnt, (uint32_t)s->pc, ut);
    return;
  }
  uint64_t pbase = (uint64_t)(int64_t)(int32_t)proc;
  char comm[33]; int ci = 0;
  for(; ci < 32; ci++) {
    uint32_t pa;
    if(!tlb_probe_ro(s, pbase + 1512 + ci, &pa)) break;
    uint8_t c = s->mem.get<uint8_t>(pa);
    if(c == 0) break;
    comm[ci] = (c >= 32 && c < 127) ? (char)c : '.';
  }
  comm[ci] = 0;
  uint32_t pid = 0; guest_rd32_be(s, pbase + 420, &pid);   /* p_pid (prgetpsinfo: proc+420) */
  bool user = (uint32_t)s->pc < 0x80000000u;
  fprintf(stderr, "[curproc] icnt=%lu pc=%08x (%s) proc=%08x pid=%u comm='%s'\n",
          (unsigned long)s->icnt, (uint32_t)s->pc, user ? "user" : "kernel",
          proc, pid, comm);
}
static void raise_syscall(state_t *s) {
  set_exc_pc(s);
  raise_common(s, 8u);
}
static void raise_break(state_t *s) {
  set_exc_pc(s);
  raise_common(s, 9u);
}

void raise_int(state_t *s, uint32_t epc) {
  s->cpr0[CPR0_EPC]   = epc;
  s->cpr0[CPR0_CAUSE] = (1u << 15);  /* IP[7]=1 (timer), ExcCode=0, BD=0 */
  s->cpr0[CPR0_SR]    = (s->cpr0[CPR0_SR] & ~SR_ERL) | SR_EXL;
  s->pc = sext32(0xBFC00180u);
}

static uint32_t getConditionCode(state_t *s, uint32_t cc) {
  return ((s->fcr1[CP1_CR25] & (1U<<cc)) >> cc) & 0x1;
}

static void setConditionCode(state_t *s, uint32_t v, uint32_t cc) {
  uint32_t m0,m1,m2;
  m0 = 1U<<cc;
  m1 = ~m0;
  m2 = ~(v-1);
  s->fcr1[CP1_CR25] = (s->fcr1[CP1_CR25] & m1) | ((1U<<cc) & m2);
}



/* ------------------------------------------------------------------------
 * Translating TLB (R4000), matching the r9999 RTL tlb.sv semantics.
 *
 * EntryHi  : R[63:62] | VPN2[39:13] | ASID[7:0]
 * EntryLo0/1: PFN[33:6] | C[5:3] | D[2] | V[1] | G[0]
 * PageMask : mask bits [24:13] (4KB..16MB pages; for 4KB pages PageMask=0)
 * Context  : PTEBase[63:23] | BadVPN2[22:4] | 0[3:0]   (BadVPN2 = VA[31:13])
 * XContext : PTEBase[63:33]|R[32:31]|BadVPN2[30:4]|0    (BadVPN2 = VA[39:13])
 * ------------------------------------------------------------------------ */
enum class tlb_op { fetch, load, store };

/* Fold the faulting VA into Context (VA[31:13]<<4) and XContext (R, VA[39:13]<<4),
 * preserving the software-written PTEBase fields. Also set BadVAddr and
 * EntryHi.VPN2 (ASID preserved) so the refill handler can build the new entry. */
static void tlb_set_fault_state(state_t *s, uint64_t va) {
  s->cpr0[CPR0_BADVADDR]     = (uint32_t)va;
  s->cpr0_64[CPR0_BADVADDR]  = va;

  /* EntryHi: VPN2 = VA[39:13], R = VA[63:62]; preserve ASID[7:0] */
  uint64_t r    = (va >> 62) & 0x3;
  uint64_t vpn2 = (va >> 13) & 0x7ffffffULL;          /* [39:13] -> 27 bits */
  uint64_t asid = s->cpr0_64[CPR0_ENTRYHI] & 0xffULL;
  uint64_t ehi  = (r << 62) | (vpn2 << 13) | asid;
  s->cpr0_64[CPR0_ENTRYHI] = ehi;
  s->cpr0[CPR0_ENTRYHI]    = (uint32_t)ehi;

  /* Context: [22:4] = VA[31:13]; preserve PTEBase [63:23] */
  uint64_t ctx = s->cpr0_64[CPR0_CONTEXT] & ~0x7fffffULL;
  ctx |= ((va >> 13) & 0x7ffffULL) << 4;              /* VA[31:13] -> [22:4] */
  s->cpr0_64[CPR0_CONTEXT] = ctx;
  s->cpr0[CPR0_CONTEXT]    = (uint32_t)ctx;

  /* XContext: [3:0]=0, [30:4]=VA[39:13] (BadVPN2), [32:31]=R; preserve PTEBase. */
  uint64_t xctx = s->cpr0_64[CPR0_XCONTEXT] & ~0x1ffffffffULL;
  xctx |= ((va >> 13) & 0x7ffffffULL) << 4;           /* BadVPN2 -> [30:4] */
  xctx |= r << 31;                                    /* R -> [32:31] */
  s->cpr0_64[CPR0_XCONTEXT] = xctx;
  s->cpr0[CPR0_XCONTEXT]    = (uint32_t)xctx;
}

/* Raise a TLB exception: Refill (no match), Invalid (V=0), or Modified.
 * is_refill picks the dedicated refill/XTLB vector when EXL==0. */
static void raise_tlb(state_t *s, uint64_t va, uint32_t exccode,
                      bool is_refill, bool is_xtlb) {
  static const bool tlbdbg = getenv("TLBDBG") != nullptr;
  bool exl_was_set = (s->cpr0[CPR0_SR] & SR_EXL) != 0;
  tlb_set_fault_state(s, va);
  static const bool kptedbg = getenv("KPTEDBG") != nullptr;
  if((uint32_t)va == 0xff800000u && kptedbg) {
    static int once = 0;
    if(once++ < 2) {
      fprintf(stderr, "[KPTE] fault on 0xff800000: faultPC=%08x EPC=%08x exl=%d code=%u refill=%d ctx=%08x icnt=%lu\n",
              (uint32_t)s->pc, s->cpr0[CPR0_EPC], exl_was_set?1:0, exccode, is_refill,
              s->cpr0[CPR0_CONTEXT], (unsigned long)s->icnt);
      for(int i = 0; i < state_t::NUM_TLB_ENTRIES; i++)
        if(s->tlb[i].entry_hi || (s->tlb[i].entry_lo0 | s->tlb[i].entry_lo1) & 0x2)
          fprintf(stderr, "   tlb[%2d] hi=%016llx lo0=%016llx lo1=%016llx pm=%08x\n", i,
                  (unsigned long long)s->tlb[i].entry_hi,
                  (unsigned long long)s->tlb[i].entry_lo0,
                  (unsigned long long)s->tlb[i].entry_lo1, s->tlb[i].page_mask);
    }
  }
  if(tlbdbg)
    fprintf(stderr, "[raise_tlb] va=%016llx code=%u refill=%d xtlb=%d pc=%08x exl=%d ctx=%08x ctx64=%016llx\n",
            (unsigned long long)va, exccode, is_refill, is_xtlb, (uint32_t)s->pc,
            exl_was_set ? 1 : 0, s->cpr0[CPR0_CONTEXT],
            (unsigned long long)s->cpr0_64[CPR0_CONTEXT]);
  set_exc_pc(s);
  s->cpr0[CPR0_CAUSE] = (s->cpr0[CPR0_CAUSE] & ~(0x1fu << 2)) | (exccode << 2);
  s->cpr0[CPR0_SR]    = (s->cpr0[CPR0_SR] & ~SR_ERL) | SR_EXL;
  s->pc = sext32(exc_vector(s, is_refill && !exl_was_set, exl_was_set, is_xtlb));
  s->tlb_fault = true;
}

/* Translate a virtual address. On a TLB exception, sets s->tlb_fault and returns
 * 0 (the caller must check s->tlb_fault and abort the instruction). Otherwise
 * returns the physical address. Unmapped segments (kseg0/kseg1, xkphys) are a
 * fast path with no lookup. */
/* Software micro-TLB: a direct-mapped cache of recent (VPN,ASID)->PPN mappings
 * in front of the 48-entry architectural TLB CAM, so the common case skips the
 * full linear scan on every fetch/load/store. The architectural s->tlb[] stays
 * the source of truth -- this is purely a sim accelerator (mirrors the
 * interp_rv64 lookup_tlb trick). Per-4KB-page keying makes it page-size
 * agnostic: ppn=pa>>12 reconstructs the right PA for any PageMask. Flushed on
 * any TLB write (TLBWI/TLBWR) -- the only ops that change a mapping -- so a hit
 * can never diverge from what the CAM would return. ASID is part of the tag, so
 * an ASID change needs no flush (stale entries simply miss and re-walk; global
 * pages get cached per-ASID). */
static const int UTLB_SZ = 64;          /* power of two */
struct utlb_entry { uint64_t vpn, asid; uint32_t ppn; bool dirty, valid; };
static utlb_entry g_utlb[UTLB_SZ];
static inline void utlb_flush() { for(auto &e : g_utlb) e.valid = false; }

static uint32_t va_translate(state_t *s, uint64_t va, tlb_op op) {
  uint32_t hi32 = (uint32_t)(va >> 32);
  uint32_t lo32 = (uint32_t)va;

  /* 32-bit compatibility segments (sign-extended VA: hi32 is 0x00000000 for
   * useg or 0xffffffff for kseg0..kseg3). */
  if(hi32 == 0x00000000u || hi32 == 0xffffffffu) {
    uint32_t seg = lo32 >> 29;
    /* kseg0 (0x80000000-0x9fffffff) and kseg1 (0xa0000000-0xbfffffff): unmapped */
    if(seg == 0x4 || seg == 0x5) {
      return lo32 & 0x1fffffff;
    }
    /* useg/kuseg (seg 0-3), ksseg/kseg2 (seg 6), kseg3 (seg 7): TLB mapped */
  } else {
    /* 64-bit address space. xkphys (bits[63:62]==2) is unmapped direct PA. */
    if(((va >> 62) & 0x3) == 0x2) {
      return (uint32_t)(va & 0xffffffffffULL);   /* low 40 bits = PA */
    }
    /* xkuseg/xksseg/xkseg etc: TLB mapped (fall through) */
  }

  /* ---- TLB lookup over the 48 entries ---- */
  /* xtlb only selects the refill exception vector (XTLB vs TLB), keyed on 64-bit
   * addressing (VA upper bits not a sign-extended 32-bit value). */
  bool xtlb = !(hi32 == 0x00000000u || hi32 == 0xffffffffu);
  uint64_t cur_asid = s->cpr0_64[CPR0_ENTRYHI] & 0xffULL;

  /* micro-TLB fast path: a hit gives the PA without scanning the CAM. A store to
   * a cached-but-not-dirty page falls through to the CAM so it raises Modified. */
  uint64_t vpn = va >> 12;
  utlb_entry &ce = g_utlb[vpn & (UTLB_SZ - 1)];
  if(ce.valid && ce.vpn == vpn && ce.asid == cur_asid &&
     !(op == tlb_op::store && !ce.dirty)) {
    return (ce.ppn << 12) | (uint32_t)(va & 0xfffULL);
  }

  /* Sail tlbEntryMatch / tlbSearch (mips_tlb.sail) -- UNCONDITIONAL full match:
   *   r=VA[63:62], vpn2=VA[39:13];
   *   valid & (r==entryR) & ((vpn2 & vpnMask)==(entryVPN & vpnMask)) & (asid|G).
   * No narrow/wide and R is ALWAYS compared (the spec has no 32-bit shortcut). */
  for(int i = 0; i < state_t::NUM_TLB_ENTRIES; i++) {
    uint64_t pm     = s->tlb[i].page_mask & 0x1ffe000ULL; /* mask bits [24:13] */
    uint64_t vpnMask= (~(uint64_t)(pm | 0x1fffULL)) & 0x000000ffffffe000ULL; /* VPN2[39:13] */
    uint64_t e_hi   = s->tlb[i].entry_hi;
    bool global     = (s->tlb[i].entry_lo0 & 1u) && (s->tlb[i].entry_lo1 & 1u);
    bool vpn_match  = ((va & vpnMask) == (e_hi & vpnMask))
                   && (((va >> 62) & 0x3) == ((e_hi >> 62) & 0x3));
    bool asid_match = global || (cur_asid == (e_hi & 0xffULL));
    if(!(vpn_match && asid_match))
      continue;

    /* A TLB entry maps an even/odd page PAIR. With PageMask, (pm|0x1fff) spans the
     * whole pair (offset+select); one page's offset is the low half of that, and
     * the even/odd select bit is the bit just above the single-page offset.
     * 4KB: pair=0x1fff -> off=0xfff (12 bits), select=0x1000 (bit 12). */
    uint64_t pair_mask = pm | 0x1fffULL;
    uint64_t off_mask  = pair_mask >> 1;           /* in-(single)page offset bits */
    uint64_t sel_bit   = (pair_mask + 1) >> 1;     /* even/odd selector bit */
    bool odd           = (va & sel_bit) != 0;
    uint64_t e_lo      = odd ? s->tlb[i].entry_lo1 : s->tlb[i].entry_lo0;

    if(!(e_lo & 0x2u)) {                            /* V == 0 -> TLB Invalid */
      uint32_t code = (op == tlb_op::store) ? 3u : 2u;
      raise_tlb(s, va, code, /*is_refill=*/false, xtlb);
      return 0;
    }
    if(op == tlb_op::store && !(e_lo & 0x4u)) {     /* D == 0 -> TLB Modified */
      raise_tlb(s, va, 1u, /*is_refill=*/false, xtlb);
      return 0;
    }
    uint64_t pfn = (e_lo >> 6) & 0xfffffffULL;      /* PFN[33:6] -> 28 bits */
    uint64_t pa  = (pfn << 12) | (va & off_mask);
    /* fill the micro-TLB for this 4KB sub-page (only reached for a V=1 hit) */
    ce.vpn = vpn; ce.asid = cur_asid; ce.ppn = (uint32_t)(pa >> 12);
    ce.dirty = (e_lo & 0x4u) != 0; ce.valid = true;
    return (uint32_t)pa;
  }

  /* No match -> TLB Refill */
  static const bool tlbdbg = getenv("TLBDBG") != nullptr;
  if(tlbdbg) {
    fprintf(stderr, "[tlb-refill] va=%016llx op=%d asid=%llx pc=%08x\n",
            (unsigned long long)va, (int)op, (unsigned long long)cur_asid,
            (uint32_t)s->pc);
    for(int i = 0; i < state_t::NUM_TLB_ENTRIES; i++) {
      if((s->tlb[i].entry_lo0 | s->tlb[i].entry_lo1) & 0x2)
        fprintf(stderr, "   tlb[%2d] hi=%016llx lo0=%016llx lo1=%016llx pm=%08x\n",
                i, (unsigned long long)s->tlb[i].entry_hi,
                (unsigned long long)s->tlb[i].entry_lo0,
                (unsigned long long)s->tlb[i].entry_lo1, s->tlb[i].page_mask);
    }
  }
  uint32_t code = (op == tlb_op::store) ? 3u : 2u;
  raise_tlb(s, va, code, /*is_refill=*/true, xtlb);
  return 0;
}

/* Read-only address probe for debug knobs: returns true and sets *pa if va maps
 * to a valid (V=1) physical address, WITHOUT raising any TLB exception (mirrors
 * va_translate's segment + 48-entry lookup, minus the fault paths). */
static bool tlb_probe_ro(state_t *s, uint64_t va, uint32_t *pa) {
  uint32_t hi32 = (uint32_t)(va >> 32);
  uint32_t lo32 = (uint32_t)va;
  if(hi32 == 0x00000000u || hi32 == 0xffffffffu) {
    uint32_t seg = lo32 >> 29;
    if(seg == 0x4 || seg == 0x5) { *pa = lo32 & 0x1fffffff; return true; }
  } else if(((va >> 62) & 0x3) == 0x2) {
    *pa = (uint32_t)(va & 0xffffffffffULL); return true;
  }
  bool wide = !(hi32 == 0x00000000u || hi32 == 0xffffffffu);
  uint64_t cur_asid = s->cpr0_64[CPR0_ENTRYHI] & 0xffULL;
  uint64_t cmp_mask = wide ? ~0x3ULL : 0xffffffffULL;
  for(int i = 0; i < state_t::NUM_TLB_ENTRIES; i++) {
    uint64_t pm     = s->tlb[i].page_mask & 0x1ffe000ULL;
    uint64_t mask   = (~(uint64_t)(pm | 0x1fffULL)) & cmp_mask;
    uint64_t e_hi   = s->tlb[i].entry_hi;
    bool global     = (s->tlb[i].entry_lo0 & 1u) && (s->tlb[i].entry_lo1 & 1u);
    bool vpn_match  = (va & mask) == (e_hi & mask);
    if(wide) vpn_match = vpn_match && (((va >> 62) & 0x3) == ((e_hi >> 62) & 0x3));
    bool asid_match = global || (cur_asid == (e_hi & 0xffULL));
    if(!(vpn_match && asid_match)) continue;
    uint64_t pair_mask = pm | 0x1fffULL;
    uint64_t off_mask  = pair_mask >> 1;
    uint64_t sel_bit   = (pair_mask + 1) >> 1;
    bool odd           = (va & sel_bit) != 0;
    uint64_t e_lo      = odd ? s->tlb[i].entry_lo1 : s->tlb[i].entry_lo0;
    if(!(e_lo & 0x2u)) return false;               /* V == 0 */
    uint64_t pfn = (e_lo >> 6) & 0xfffffffULL;
    *pa = (uint32_t)((pfn << 12) | (va & off_mask));
    return true;
  }
  return false;
}

template <typename T>
struct c1xExec {
  void operator()(const coproc1x_t& insn, state_t *s) {
    T _fr = *reinterpret_cast<T*>(s->cpr1+insn.fr);
    T _fs = *reinterpret_cast<T*>(s->cpr1+insn.fs);
    T _ft = *reinterpret_cast<T*>(s->cpr1+insn.ft);
    T &_fd = *reinterpret_cast<T*>(s->cpr1+insn.fd);  
    switch(insn.id)
      {
      case 4:
	_fd = _fs*_ft + _fr;
	break;
      case 5:
	_fd = _fs*_ft - _fr;
	break;
      default:
	std::cerr << "unhandled coproc1x insn @ 0x"
		  << std::hex << s->pc << std::dec
		  << ", id = " << insn.id
		  <<"\n";
	exit(-1);
      }
    s->pc += 4;
  }
};


template <bool EL, typename T>
void lxc1(uint32_t inst, state_t *s) {
  mips_t mi(inst);
  uint32_t ea = va_translate(s, s->gpr[mi.lc1x.base] + s->gpr[mi.lc1x.index], tlb_op::load);
  if(s->tlb_fault) return;
  *reinterpret_cast<T*>(s->cpr1 + mi.lc1x.fd) = bswap<EL>(s->mem.get<T>(ea));
  s->pc += 4;
}

template <bool EL>
static void execCoproc1x(uint32_t inst, state_t *s) {
  mips_t mi(inst);

  switch(mi.lc1x.id)
    {
    case 0:
      //lwxc1
      lxc1<EL,int32_t>(inst, s);
      return;
    case 1:
      //ldxc1
      lxc1<EL,int64_t>(inst, s);
      return;
    default:
      break;
    }
  
  switch(mi.c1x.fmt)
   {
   case 0: {
     c1xExec<float> e;
     e(mi.c1x, s);
     return;
   }
   case 1: {
     c1xExec<double> e;
     e(mi.c1x, s);
     return;
   }
   default:
     std::cerr << "weird type in do_c1x_op @ 0x"
	       << std::hex << s->pc << std::dec
	       <<"\n";
     exit(-1);
   }
}



/* FASTDELAY: collapse calibrated busy-delay loops (e.g. IRIX us_delay/delayloop,
 * Linux __delay). A delay loop is a conditional branch back to its OWN address
 * (imm==-4: 2-insn loop = branch + its delay slot) that compares ONE register
 * against zero; the delay slot only decrements/increments that register, touching
 * no memory or device. Spinning the full faithful count is hundreds of millions
 * of ISS instructions per call. When enabled, clamp the counter to the exit
 * boundary so the loop terminates in one more iteration (the delay slot still
 * does the final step, so the architectural state stays consistent). Off by
 * default -- it skips wall-time the guest intended to burn, so leave it off for
 * cycle-accurate co-sim. */
static const bool g_fastdelay = getenv("FASTDELAY") != nullptr;

template <bool EL, branch_type bt>
void branch(uint32_t inst, state_t *s) {
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
  state_t::reg_t npc = s->pc+4; 
  bool isLikely = false, takeBranch = false, saveReturn = false;
  switch(bt)
    {
    case branch_type::beql:
      takeBranch = (s->gpr[rt] == s->gpr[rs]);
      HISTO(s, mipsInsn::BEQL);
      isLikely = true;
      break;
    case branch_type::beq:
      takeBranch = (s->gpr[rt] == s->gpr[rs]);
      HISTO(s, mipsInsn::BEQ);
      break;
    case branch_type::bnel:
      isLikely = true;
      takeBranch = (s->gpr[rt] != s->gpr[rs]);
      HISTO(s, mipsInsn::BNEL);
      break;
    case branch_type::bne:
      takeBranch = (s->gpr[rt] != s->gpr[rs]);
      HISTO(s, mipsInsn::BNE);
      break;
    case branch_type::blezl:
      isLikely = true;
      takeBranch = (s->gpr[rs] <= 0);
      HISTO(s, mipsInsn::BLEZL);
      break;
    case branch_type::blez:
      takeBranch = (s->gpr[rs] <= 0);
      HISTO(s, mipsInsn::BLEZ);
      break;
    case branch_type::bgtzl:
      isLikely = true;
      takeBranch = (s->gpr[rs] > 0);
      HISTO(s, mipsInsn::BGTZL);
      break;
    case branch_type::bgtz:
      takeBranch = (s->gpr[rs] > 0);
      HISTO(s, mipsInsn::BGTZ);
      break;
    case branch_type::bgezl:
      isLikely = true;
      takeBranch = (s->gpr[rs] >= 0);
      HISTO(s, mipsInsn::BGEZL);
      break;      
    case branch_type::bgez:
      takeBranch = (s->gpr[rs] >= 0);
      HISTO(s, mipsInsn::BGEZ);
      break;
    case branch_type::bltzl:
      isLikely = true;
      takeBranch = (s->gpr[rs] < 0);
      HISTO(s, mipsInsn::BLTZL);
      break;
    case branch_type::bltz:
      takeBranch = (s->gpr[rs] < 0);
      HISTO(s, mipsInsn::BLTZ);
      break;
    case branch_type::bgezal:
      takeBranch = (s->gpr[rs] >= 0);
      HISTO(s, mipsInsn::BGEZAL);
      saveReturn = true;
      break;
    case branch_type::bltzal:
      takeBranch = (s->gpr[rs] < 0);
      HISTO(s, mipsInsn::BLTZAL);
      saveReturn = true;
      break;
    case branch_type::bgezall:
      isLikely = true;
      takeBranch = (s->gpr[rs] >= 0);
      HISTO(s, mipsInsn::BGEZALL);
      saveReturn = true;
      break;
    case branch_type::bltzall:
      isLikely = true;
      takeBranch = (s->gpr[rs] < 0);
      HISTO(s, mipsInsn::BLTZALL);
      saveReturn = true;
      break;
    case branch_type::bc1tl:
      isLikely = true;
      takeBranch = getConditionCode(s,((inst>>18)&7))==1;
      HISTO(s, mipsInsn::BC1TL);
      break;
    case branch_type::bc1t:
      takeBranch = getConditionCode(s,((inst>>18)&7))==1;
      HISTO(s, mipsInsn::BC1T);
      break;
    case branch_type::bc1fl:
      isLikely = true;
      takeBranch = getConditionCode(s,((inst>>18)&7))==0;
      HISTO(s, mipsInsn::BC1FL);
      break;
    case branch_type::bc1f:
      takeBranch = getConditionCode(s,((inst>>18)&7))==0;
      HISTO(s, mipsInsn::BC1F);
      break;
    default:
      UNREACHABLE();
    }

  /* FASTDELAY: self-branch compare-against-zero delay loop -> clamp the counter
   * to the exit boundary so the (large) calibrated spin collapses to one more
   * iteration. The delay slot below performs the final decrement/increment. */
  if(g_fastdelay && takeBranch && !isLikely && imm == -4) {
    state_t::reg_t v = s->gpr[rs];
    switch(bt) {
    case branch_type::bgtz: if(v >  0x10000) s->gpr[rs] =  1; break; /* exit rs<=0 */
    case branch_type::bltz: if(v < -0x10000) s->gpr[rs] = -1; break; /* exit rs>=0 */
    case branch_type::blez: if(v < -0x10000) s->gpr[rs] =  0; break; /* exit rs>0  */
    case branch_type::bgez: if(v >  0x10000) s->gpr[rs] =  0; break; /* exit rs<0  */
    default: break;
    }
  }

  s->pc += 4;
  if(isLikely) {
    if(takeBranch) {
      if(saveReturn)
	s->gpr[31] = npc + 4;   /* full 64-bit link (n64 user PCs exceed 32 bits) */
      if(!run_delay_slot<EL>(s))
	s->pc = (imm+npc);
    }
    else {
      s->pc += 4;
    }
  }
  else {
    bool ds_faulted = run_delay_slot<EL>(s);
    if(takeBranch){
      if(saveReturn) {
	s->gpr[31] = npc + 4;   /* full 64-bit link (n64 user PCs exceed 32 bits) */
      }
      if(!ds_faulted)
	s->pc = (imm+npc);
    }
  }
}

template <bool EL>
void _bgez_bltz(uint32_t inst, state_t *s) {
  uint32_t rt = (inst >> 16) & 31;
  switch(rt)
    {
    case 0:
      branch<EL,branch_type::bltz>(inst, s);
      break;
    case 1:
      branch<EL,branch_type::bgez>(inst, s);
      break;
    case 2:
      branch<EL,branch_type::bltzl>(inst, s);
      break;
    case 3:
      branch<EL,branch_type::bgezl>(inst, s);
      break;
    case 17:
      branch<EL,branch_type::bgezal>(inst, s);
      break;
    case 16:
      branch<EL,branch_type::bltzal>(inst, s);
      break;
    case 18:
      branch<EL,branch_type::bltzall>(inst, s);
      break;
    case 19:
      branch<EL,branch_type::bgezall>(inst, s);
      break;
    case 8: case 9: case 10: case 11: case 12: case 14: { /* trap-immediates */
      uint32_t rs = (inst >> 21) & 31;
      int64_t a = (int64_t)s->gpr[rs];
      int64_t simm = (int64_t)(int16_t)(inst & 0xffff);
      bool trap = false;
      switch(rt) {
      case 8:  trap = (a >= simm);                           HISTO(s, mipsInsn::TGEI);  break;
      case 9:  trap = ((uint64_t)a >= (uint64_t)simm);       HISTO(s, mipsInsn::TGEIU); break;
      case 10: trap = (a < simm);                            HISTO(s, mipsInsn::TLTI);  break;
      case 11: trap = ((uint64_t)a < (uint64_t)simm);        HISTO(s, mipsInsn::TLTIU); break;
      case 12: trap = (a == simm);                           HISTO(s, mipsInsn::TEQI);  break;
      case 14: trap = (a != simm);                           HISTO(s, mipsInsn::TNEI);  break;
      }
      if(trap) raise_trap(s); else s->pc += 4;
      break;
    }
    default:
      std::cerr << "case " << rt << " not handled!\n";
      exit(-1);
    }
}


template <bool EL>
void _lw(uint32_t inst, state_t *s) {
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  uint32_t ea = va_translate(s, s->gpr[rs] + imm, tlb_op::load);
  if(s->tlb_fault) return;
  if(ea & 3) { raise_adel(s); return; }
  s->gpr[rt] = bswap<EL>(s->mem.get<int32_t>(ea));
  //#define TRACE_MEM
  //printf("_lw pc %x from ea %x = %x\n", s->pc, (uint32_t)s->gpr[rs] + imm,
  //s->gpr[rt]);
  //#undef TRACE_MEM
  s->pc += 4;
  HISTO(s, mipsInsn::LW);
}

template <bool EL>
void _lh(uint32_t inst, state_t *s) {
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;

  uint32_t ea = va_translate(s, s->gpr[rs] + imm, tlb_op::load);
  if(s->tlb_fault) return;
  if(ea & 1) { raise_adel(s); return; }
  int16_t mem = bswap<EL>(s->mem.get<int16_t>(ea));
  
  s->gpr[rt] = static_cast<int32_t>(mem);
#ifdef TRACE_MEM
  printf("_lh from %x = %x\n", ea, s->gpr[rt]);
#endif
  s->pc +=4;
  HISTO(s, mipsInsn::LH);  
}


static void _lb(uint32_t inst, state_t *s){
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  
  uint32_t ea = va_translate(s, s->gpr[rs] + imm, tlb_op::load);
  if(s->tlb_fault) return;
  s->gpr[rt] = static_cast<int32_t>(s->mem.get<int8_t>(ea));
#ifdef TRACE_MEM
  printf("_lb from %x = %x\n", ea, s->gpr[rt]);
#endif  
  s->pc += 4;
  HISTO(s, mipsInsn::LB);  
}

static void _lbu(uint32_t inst, state_t *s){
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;

  uint32_t ea = va_translate(s, s->gpr[rs] + imm, tlb_op::load);
  if(s->tlb_fault) return;
  uint32_t zExt = s->mem.get<uint8_t>(ea);
  *((uint64_t*)&(s->gpr[rt])) = zExt;
  s->pc += 4;
  HISTO(s, mipsInsn::LBU);
}


template <bool EL>
void _lhu(uint32_t inst, state_t *s) {
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;

  uint32_t ea = va_translate(s, s->gpr[rs] + imm, tlb_op::load);
  if(s->tlb_fault) return;
  if(ea & 1) { raise_adel(s); return; }
  uint32_t zExt = bswap<EL>(s->mem.get<uint16_t>(ea));
  *((uint64_t*)&(s->gpr[rt])) = zExt;
  //printf("_lhu from %x = %x\n", ea, s->gpr[rt]);  
  s->pc += 4;
  HISTO(s, mipsInsn::LHU);  
}


template <bool EL>
void _sw(uint32_t inst, state_t *s) {
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  uint32_t ea = va_translate(s, s->gpr[rs] + imm, tlb_op::store);
  if(s->tlb_fault) return;
  if(ea & 3) { raise_ades(s); return; }
  s->mem.set<int32_t>(ea,  bswap<EL>(static_cast<int32_t>(s->gpr[rt])));
  s->pc += 4;
  HISTO(s, mipsInsn::SW);
}

template <bool EL>
void _sd(uint32_t inst, state_t *s) {
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int32_t imm = (int32_t)(int16_t)(inst & 0xffffu);
  uint32_t ea = va_translate(s, s->gpr[rs] + imm, tlb_op::store);
  if(s->tlb_fault) return;
  if(ea & 7) { raise_ades(s); return; }
  s->mem.set<int64_t>(ea, bswap<EL>(s->gpr[rt]));
  s->pc += 4;
  HISTO(s, mipsInsn::SD);
}

template <bool EL>
void _ld(uint32_t inst, state_t *s) {
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int32_t imm = (int32_t)(int16_t)(inst & 0xffffu);
  uint32_t ea = va_translate(s, s->gpr[rs] + imm, tlb_op::load);
  if(s->tlb_fault) return;
  if(ea & 7) { raise_adel(s); return; }
  s->gpr[rt] = bswap<EL>(s->mem.get<int64_t>(ea));
  s->pc += 4;
  HISTO(s, mipsInsn::LD);
}

template <bool EL>
void _sc(uint32_t inst, state_t *s) {
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  uint32_t ea = va_translate(s, s->gpr[rs] + imm, tlb_op::store);
  if(s->tlb_fault) return;
  s->mem.set<int32_t>(ea,  bswap<EL>(static_cast<int32_t>(s->gpr[rt])));
  s->gpr[rt] = 1;
  s->pc += 4;
  HISTO(s, mipsInsn::SC);
}

template <bool EL>
void _scd(uint32_t inst, state_t *s) {
  /* store conditional doubleword (mirrors _sc; single-core functional sim, so
   * the store always succeeds and rt is set to 1) */
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  uint32_t ea = va_translate(s, s->gpr[rs] + imm, tlb_op::store);
  if(s->tlb_fault) return;
  s->mem.set<int64_t>(ea, bswap<EL>(static_cast<int64_t>(s->gpr[rt])));
  s->gpr[rt] = 1;
  s->pc += 4;
  HISTO(s, mipsInsn::SC);
}


template <bool EL>
void _sh(uint32_t inst, state_t *s) {
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
    
  uint32_t ea = va_translate(s, s->gpr[rs] + imm, tlb_op::store);
  if(s->tlb_fault) return;
  if(ea & 1) { raise_ades(s); return; }
  s->mem.set<int16_t>(ea,  bswap<EL>(((int16_t)s->gpr[rt])));
  s->pc += 4;
  HISTO(s, mipsInsn::SH);
}

static void _sb(uint32_t inst, state_t *s) {
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
    
  uint32_t ea = va_translate(s, s->gpr[rs] + imm, tlb_op::store);
  if(s->tlb_fault) return;
  s->mem.set<uint8_t>(ea, static_cast<uint8_t>(s->gpr[rt]));
  
  s->pc +=4;
  HISTO(s, mipsInsn::SB);
}

static void _mtc1(uint32_t inst, state_t *s) {
  uint32_t fs = (inst>>11) & 31;
  uint32_t rt = (inst>>16) & 31;
  /* mips3/4 FR=1: FPR[fs] = sign_extend32(GPR[rt][31:0]) */
  s->cpr1[fs] = (uint64_t)(int64_t)(int32_t)(uint32_t)s->gpr[rt];
  s->pc += 4;
  HISTO(s, mipsInsn::MTC1);
}

static void _mfc1(uint32_t inst, state_t *s) {
  uint32_t fs = (inst>>11) & 31;
  uint32_t rt = (inst>>16) & 31;
  /* mips3/4 FR=1: GPR[rt] = sign_extend32(FPR[fs][31:0]) */
  s->gpr[rt] = (int64_t)(int32_t)(uint32_t)s->cpr1[fs];
  s->pc +=4;
  HISTO(s, mipsInsn::MFC1);
}

static void _dmtc1(uint32_t inst, state_t *s) {
  uint32_t fs = (inst>>11) & 31;
  uint32_t rt = (inst>>16) & 31;
  /* FR=1: FPR[fs] = GPR[rt] (full 64-bit, no sign-ext) */
  s->cpr1[fs] = s->gpr[rt];
  s->pc += 4;
  HISTO(s, mipsInsn::DMTC1);
}

static void _dmfc1(uint32_t inst, state_t *s) {
  uint32_t fs = (inst>>11) & 31;
  uint32_t rt = (inst>>16) & 31;
  /* FR=1: GPR[rt] = FPR[fs] (full 64-bit) */
  s->gpr[rt] = s->cpr1[fs];
  s->pc += 4;
  HISTO(s, mipsInsn::DMFC1);
}

/* map a raw FP control-register number to the compact fcr1[] index */
static inline int fcr_index(uint32_t cr) {
  switch(cr) {
  case 0:  return CP1_CR0;   /* FIR  */
  case 31: return CP1_CR31;  /* FCSR */
  case 25: return CP1_CR25;
  case 26: return CP1_CR26;
  case 28: return CP1_CR28;
  default: return CP1_CR31;
  }
}

static void _cfc1(uint32_t inst, state_t *s) {
  uint32_t cr = (inst>>11) & 31;
  uint32_t rt = (inst>>16) & 31;
  /* GPR[rt] = sign_extend32(FCR[cr]); FCR0 is the read-only FIR */
  uint32_t v = (cr == 0) ? 0x00000500u : (uint32_t)s->fcr1[fcr_index(cr)];
  s->gpr[rt] = (int64_t)(int32_t)v;
  s->pc += 4;
  HISTO(s, mipsInsn::CFC1);
}

static void _ctc1(uint32_t inst, state_t *s) {
  uint32_t cr = (inst>>11) & 31;
  uint32_t rt = (inst>>16) & 31;
  /* FCR[cr] = GPR[rt][31:0]; FCR0 (FIR) is read-only */
  if(cr != 0)
    s->fcr1[fcr_index(cr)] = (uint32_t)s->gpr[rt];
  s->pc += 4;
  HISTO(s, mipsInsn::CTC1);
}


template <bool EL>
void _swl(uint32_t inst, state_t *s) {
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  uint32_t ea = va_translate(s, s->gpr[rs] + imm, tlb_op::store);
  if(s->tlb_fault) return;
  uint32_t ma = ea & 3;
  ea &= 0xfffffffc;
  if(EL)
    ma = 3 - ma;
  uint32_t r = bswap<EL>(s->mem.get<uint32_t>(ea));   
  uint32_t xx=0,x = s->gpr[rt];
  
  uint32_t xs = x >> (8*ma);
  /* 64-bit shift: at ma==0 the count is 32, a 32-bit-shift UB (x86 masks to 0,
   * making m=0xffffffff and storing r|rt instead of rt). */
  uint32_t m = (uint32_t)~(((uint64_t)1u << (8*(4 - ma))) - 1);
  xx = (r & m) | xs;
  //std::cout << "SIM SWL EA " << std::hex << ea
  //<< ", MA = " << ma
  //<< ", X = " << x
  //<< ", R = " << r
  //<< ", M = " << m
  //	    << ", XX = " << xx << std::dec << "\n";
  
  s->mem.set<uint32_t>(ea, bswap<EL>(xx));
  s->pc += 4;
  HISTO(s, mipsInsn::SWL);  
}

template <bool EL>
void _swr(uint32_t inst, state_t *s) {
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  uint32_t ea = va_translate(s, s->gpr[rs] + imm, tlb_op::store);
  if(s->tlb_fault) return;
  uint32_t ma = ea & 3;
  if(EL)
    ma = 3 - ma;
  ea &= ~(3U);
  uint32_t r = bswap<EL>(s->mem.get<uint32_t>(ea));   
  uint32_t xx=0,x = s->gpr[rt];
  
  uint32_t xs = 8*(3-ma);
  uint32_t rm = (1U << xs) - 1;

  xx = (x << xs) | (rm & r);
  s->mem.set<uint32_t>(ea, bswap<EL>(xx));
  s->pc += 4;
  HISTO(s, mipsInsn::SWR);
}

template <bool EL>
void _lwl(uint32_t inst, state_t *s) {
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  
  uint32_t ea = va_translate(s, s->gpr[rs] + imm, tlb_op::load);
  if(s->tlb_fault) return;
  uint32_t u_ea = ea;
  uint32_t ma = ea & 3;
  ea &= 0xfffffffc;
  if(EL)
    ma = 3 - ma;
  uint32_t r = bswap<EL>(s->mem.get<uint32_t>(ea));
  state_t::reg_t x =  s->gpr[rt];
  
  switch(ma)
    {
    case 0:
      s->gpr[rt] = sext64(r);
      break;
    case 1:
      s->gpr[rt] = sext64(((r & 0x00ffffff) << 8) | (x & 0x000000ff)) ;
      break;
    case 2:
      s->gpr[rt] = sext64(((r & 0x0000ffff) << 16)  | (x & 0x0000ffff)) ;
      break;
    case 3:
      s->gpr[rt] = sext64(((r & 0x00ffffff) << 24)  | (x & 0x00ffffff));
      break;
    }
#ifdef TRACE_MEM
  printf("_lwl from %x = %x\n", u_ea, s->gpr[rt]);
#endif  
  s->pc += 4;
  HISTO(s, mipsInsn::LWL);  
}

template<bool EL>
void _lwr(uint32_t inst, state_t *s) {
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
 
  uint32_t ea = va_translate(s, s->gpr[rs] + imm, tlb_op::load);
  if(s->tlb_fault) return;
  uint32_t u_ea = ea;
  uint32_t ma = ea & 3;
  ea &= 0xfffffffc;
  if(EL)
    ma = 3-ma;

  uint32_t r = bswap<EL>(s->mem.get<uint32_t>(ea));
  state_t::reg_t x =  s->gpr[rt];
  
  switch(ma)
    {
    case 0:
      s->gpr[rt] = sext64((x & 0xffffff00) | (r>>24));
      break;
    case 1:
      s->gpr[rt] = sext64((x & 0xffff0000) | (r>>16));
      break;
    case 2:
      s->gpr[rt] = sext64((x & 0xff000000) | (r>>8));
      break;
    case 3:
      s->gpr[rt] = sext64(r);
      break;
    }

#ifdef TRACE_MEM
  printf("_lwr from %x = %x (x=%x, r = %x)\n", u_ea, s->gpr[rt], x, r);
#endif  
  
  s->pc += 4;
  HISTO(s, mipsInsn::LWR);
}

template <bool EL>
void _ldl(uint32_t inst, state_t *s) {
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int32_t imm = (int32_t)(int16_t)(inst & 0xffff);
  uint32_t ea = va_translate(s, s->gpr[rs] + imm, tlb_op::load);
  if(s->tlb_fault) return;
  uint32_t ma = ea & 7;
  ea &= ~7u;
  if(EL) ma = 7 - ma;
  uint64_t r = bswap<EL>(s->mem.get<uint64_t>(ea));
  uint64_t x = s->gpr[rt];
  /* Load (8-ma) bytes from positions [ma..7] of aligned dword into rt[63:ma*8].
   * In BE: r[63:56]=pos0, r[7:0]=pos7.  Bytes [ma..7] = r[(8-ma)*8-1:0].
   * Shift them left by ma*8 to place at rt[63:ma*8]. */
  switch(ma) {
    case 0: s->gpr[rt] = r; break;
    case 1: s->gpr[rt] = (r & 0x00ffffffffffffffULL) << 8  | (x & 0xffULL); break;
    case 2: s->gpr[rt] = (r & 0x0000ffffffffffffULL) << 16 | (x & 0xffffULL); break;
    case 3: s->gpr[rt] = (r & 0x000000ffffffffffULL) << 24 | (x & 0xffffffULL); break;
    case 4: s->gpr[rt] = (r & 0x00000000ffffffffULL) << 32 | (x & 0xffffffffULL); break;
    case 5: s->gpr[rt] = (r & 0x0000000000ffffffULL) << 40 | (x & 0xffffffffffULL); break;
    case 6: s->gpr[rt] = (r & 0x000000000000ffffULL) << 48 | (x & 0xffffffffffffULL); break;
    case 7: s->gpr[rt] = (r & 0x00000000000000ffULL) << 56 | (x & 0x00ffffffffffffffULL); break;
  }
  s->pc += 4;
  HISTO(s, mipsInsn::LDL);
}

template <bool EL>
void _ldr(uint32_t inst, state_t *s) {
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int32_t imm = (int32_t)(int16_t)(inst & 0xffff);
  uint32_t ea = va_translate(s, s->gpr[rs] + imm, tlb_op::load);
  if(s->tlb_fault) return;
  uint32_t ma = ea & 7;
  ea &= ~7u;
  if(EL) ma = 7 - ma;
  uint64_t r = bswap<EL>(s->mem.get<uint64_t>(ea));
  uint64_t x = s->gpr[rt];
  /* Load (ma+1) bytes from positions [0..ma] of aligned dword into rt[(ma+1)*8-1:0].
   * Bytes [0..ma] = r[63:63-ma*8].  Shift right by (7-ma)*8 to place at rt[(ma+1)*8-1:0]. */
  switch(ma) {
    case 0: s->gpr[rt] = (x & 0xffffffffffffff00ULL) | (r >> 56); break;
    case 1: s->gpr[rt] = (x & 0xffffffffffff0000ULL) | (r >> 48); break;
    case 2: s->gpr[rt] = (x & 0xffffffffff000000ULL) | (r >> 40); break;
    case 3: s->gpr[rt] = (x & 0xffffffff00000000ULL) | (r >> 32); break;
    case 4: s->gpr[rt] = (x & 0xffffff0000000000ULL) | (r >> 24); break;
    case 5: s->gpr[rt] = (x & 0xffff000000000000ULL) | (r >> 16); break;
    case 6: s->gpr[rt] = (x & 0xff00000000000000ULL) | (r >>  8); break;
    case 7: s->gpr[rt] = r; break;
  }
  s->pc += 4;
  HISTO(s, mipsInsn::LDR);
}

template <bool EL>
void _sdl(uint32_t inst, state_t *s) {
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int32_t imm = (int32_t)(int16_t)(inst & 0xffff);
  uint32_t ea = va_translate(s, s->gpr[rs] + imm, tlb_op::store);
  if(s->tlb_fault) return;
  uint32_t ma = ea & 7;
  ea &= ~7u;
  if(EL) ma = 7 - ma;
  uint64_t r = bswap<EL>(s->mem.get<uint64_t>(ea));
  uint64_t x = s->gpr[rt];
  /* SDL: store x's high (8-ma) bytes at memory positions [ma..7];
   * preserve memory positions [0..ma-1].
   * xs = x >> (ma*8) places x[63:ma*8] at bits [63-ma*8:0].
   * m masks the top ma bytes to preserve from memory. */
  uint64_t xs = x >> (8 * ma);
  uint64_t m  = (ma == 0) ? 0ULL : (-(1ULL << (8 * (8 - ma))));
  uint64_t merged = (r & m) | xs;
  s->mem.set<uint64_t>(ea, bswap<EL>(merged));
  s->pc += 4;
  HISTO(s, mipsInsn::SDL);
}

template <bool EL>
void _sdr(uint32_t inst, state_t *s) {
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int32_t imm = (int32_t)(int16_t)(inst & 0xffff);
  uint32_t ea = va_translate(s, s->gpr[rs] + imm, tlb_op::store);
  if(s->tlb_fault) return;
  uint32_t ma = ea & 7;
  ea &= ~7u;
  if(EL) ma = 7 - ma;
  uint64_t r = bswap<EL>(s->mem.get<uint64_t>(ea));
  uint64_t x = s->gpr[rt];
  /* SDR: store x's low (ma+1) bytes at memory positions [0..ma];
   * preserve memory positions [ma+1..7].
   * Shift x left by (7-ma)*8 to align x's low bytes to the high positions.
   * rm masks the low bytes to preserve from memory. */
  uint32_t xs_bits = 8 * (7 - ma);
  uint64_t rm = (xs_bits == 0) ? 0ULL : ((1ULL << xs_bits) - 1);
  uint64_t merged = (x << xs_bits) | (rm & r);
  s->mem.set<uint64_t>(ea, bswap<EL>(merged));
  s->pc += 4;
  HISTO(s, mipsInsn::SDR);
}

static inline char* get_open_string(sparse_mem &mem, uint32_t offset) {
  size_t len = 0;
  char *ptr = reinterpret_cast<char*>(mem.get_raw_ptr(offset));
  char *buf = nullptr;
  while(*ptr != '\0') {
    ptr++;
    len++;
  }
  buf = new char[len+1];
  memset(buf, 0, len+1);
  ptr = reinterpret_cast<char*>(mem.get_raw_ptr(offset));
  for(size_t i = 0; i < len; i++) {
    buf[i] = *ptr;
    ptr++;
  }
  return buf;
}



template <bool EL>
void _ldc1(uint32_t inst, state_t *s) {
  uint32_t ft = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  uint32_t ea = va_translate(s, s->gpr[rs] + imm, tlb_op::load);
  if(s->tlb_fault) return;
  *reinterpret_cast<int64_t*>(s->cpr1 + ft) = bswap<EL>(s->mem.get<int64_t>(ea));
  s->pc += 4;
  HISTO(s, mipsInsn::LDC1);
}

template <bool EL>
void _sdc1(uint32_t inst, state_t *s) {
  uint32_t ft = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  uint32_t ea = va_translate(s, s->gpr[rs] + imm, tlb_op::store);
  if(s->tlb_fault) return;
  s->mem.set<int64_t>(ea,  bswap<EL>((*(int64_t*)(s->cpr1 + ft))));
  s->pc += 4;
  HISTO(s, mipsInsn::SDC1);  
}

template <bool EL>
void _lwc1(uint32_t inst, state_t *s) {
  uint32_t ft = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  uint32_t ea = va_translate(s, s->gpr[rs] + imm, tlb_op::load);
  if(s->tlb_fault) return;
  uint32_t v = bswap<EL>(s->mem.get<uint32_t>(ea)); 
  *((float*)(s->cpr1 + ft)) = *((float*)&v);
  s->pc += 4;
  HISTO(s, mipsInsn::LWC1);
}

template <bool EL>
void _swc1(uint32_t inst, state_t *s) {
  uint32_t ft = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  uint32_t ea = va_translate(s, s->gpr[rs] + imm, tlb_op::store);
  if(s->tlb_fault) return;
  uint32_t v = *((uint32_t*)(s->cpr1+ft));
  s->mem.set<uint32_t>(ea, bswap<EL>(v));
  s->pc += 4;
  HISTO(s, mipsInsn::SWC1);
}

static void _truncl(uint32_t inst, state_t *s) {
  uint32_t fmt = (inst >> 21) & 31;
  uint32_t fd = (inst>>6) & 31;
  uint32_t fs = (inst>>11) & 31;
  int64_t *ptr = ((int64_t*)(s->cpr1 + fd));   /* trunc.l: 64-bit fixed result */
  switch(fmt)
    {
    case FMT_S:
      *ptr = (int64_t)(*((float*)(s->cpr1 + fs)));
      break;
    case FMT_D:
      *ptr = (int64_t)(*((double*)(s->cpr1 + fs)));
      break;
    default:
      printf("%s @ %d: unhandled fmt=%u inst=%08x pc=%08x\n", __func__, __LINE__, fmt, inst, (uint32_t)s->pc);
      exit(-1);
      break;
    }
  s->cpr1_state[fd] = fp_reg_state::dp;
  s->pc += 4;
}

static void _truncw(uint32_t inst, state_t *s) {
  uint32_t fmt = (inst >> 21) & 31;
  uint32_t fd = (inst>>6) & 31;
  uint32_t fs = (inst>>11) & 31;
  float f;
  double d;
  int32_t *ptr = ((int32_t*)(s->cpr1 + fd));
  if(currFpMode != fpMode::mips3) {
    assert((fd & 1) == 0);
    assert((fs & 1) == 0);
  }  
  switch(fmt)
    {
    case FMT_S:
      f = (*((float*)(s->cpr1 + fs)));
      //printf("f=%g\n", f);
      *ptr = (int32_t)f;
      HISTO(s, mipsInsn::TRUNC_SP_W);
      break;
    case FMT_D:
      d = (*((double*)(s->cpr1 + fs)));
      *ptr = (int32_t)d;
      HISTO(s, mipsInsn::TRUNC_DP_W);      
      //printf("id=%d\n", *ptr);
      break;
    default:
      printf("unknown trunc for fmt %d\n", fmt);
      exit(-1);
      break;
    }
  if(currFpMode != fpMode::mips3) {
    s->cpr1[fd + 1] = 0;
  }      
  s->pc += 4;
}

static void _movnd(uint32_t inst, state_t *s) {
  uint32_t fd = (inst>>6) & 31;
  uint32_t fs = (inst>>11) & 31;
  uint32_t rt = (inst>>16) & 31;
  bool notZero = (s->gpr[rt] != 0);
  s->cpr1[fd+0] = notZero ? s->cpr1[fs+0] : s->cpr1[fd+0];
  if(currFpMode != fpMode::mips3)   /* FR=0 only: the double's high word is a separate reg */
    s->cpr1[fd+1] = notZero ? s->cpr1[fs+1] : s->cpr1[fd+1];
  s->pc += 4;
}

static void _movns(uint32_t inst, state_t *s) {
  uint32_t fd = (inst>>6) & 31;
  uint32_t fs = (inst>>11) & 31;
  uint32_t rt = (inst>>16) & 31;
  bool notZero = (s->gpr[rt] != 0);
  s->cpr1[fd+0] = notZero ? s->cpr1[fs+0] : s->cpr1[fd+0];
  s->pc += 4;
}

static void _movzd(uint32_t inst, state_t *s) {
  uint32_t fd = (inst>>6) & 31;
  uint32_t fs = (inst>>11) & 31;
  uint32_t rt = (inst>>16) & 31;
 
  s->cpr1[fd+0] = (s->gpr[rt] == 0) ? s->cpr1[fs+0] : s->cpr1[fd+0];
  if(currFpMode != fpMode::mips3)   /* FR=0 only: the double's high word is a separate reg */
    s->cpr1[fd+1] = (s->gpr[rt] == 0) ? s->cpr1[fs+1] : s->cpr1[fd+1];
  s->pc += 4;
}

static void _movzs(uint32_t inst, state_t *s) {
  uint32_t fd = (inst>>6) & 31;
  uint32_t fs = (inst>>11) & 31;
  uint32_t rt = (inst>>16) & 31;

  s->cpr1[fd+0] = (s->gpr[rt] == 0) ? s->cpr1[fs+0] : s->cpr1[fd+0];
  s->pc += 4;
}

static void _movcd(uint32_t inst, state_t *s) {
  uint32_t cc = (inst >> 18) & 7;
  uint32_t fd = (inst>>6) & 31;
  uint32_t fs = (inst>>11) & 31;
  uint32_t tf = (inst>>16) & 1;

  if(tf==0) {
    if(getConditionCode(s,cc)==0) {
      s->cpr1[fd+0] = s->cpr1[fs+0];
      if(currFpMode != fpMode::mips3) s->cpr1[fd+1] = s->cpr1[fs+1];
    }
    HISTO(s, mipsInsn::FP_MOVF);
  }
  else {
    if(getConditionCode(s,cc)==1) {
      s->cpr1[fd+0] = s->cpr1[fs+0];
      if(currFpMode != fpMode::mips3) s->cpr1[fd+1] = s->cpr1[fs+1];
    }
    HISTO(s, mipsInsn::FP_MOVT);
  }
  s->pc += 4;
}

static void _movcs(uint32_t inst, state_t *s) {
  uint32_t cc = (inst >> 18) & 7;
  uint32_t fd = (inst>>6) & 31;
  uint32_t fs = (inst>>11) & 31;
  uint32_t tf = (inst>>16) & 1;
  if(tf==0) {
    s->cpr1[fd+0] = getConditionCode(s, cc) ? s->cpr1[fd+0] : s->cpr1[fs+0];
    HISTO(s, mipsInsn::FP_MOVF);
  }
  else {
    s->cpr1[fd+0] = getConditionCode(s, cc) ? s->cpr1[fs+0] : s->cpr1[fd+0];
    HISTO(s, mipsInsn::FP_MOVT);
  }
  s->pc += 4;
}


static void _movci(uint32_t inst, state_t *s) {
  uint32_t cc = (inst >> 18) & 7;
  uint32_t tf = (inst>>16) & 1;
  uint32_t rd = (inst>>11) & 31;
  uint32_t rs = (inst >> 21) & 31;
  if(tf==0) {
    /* movf */
    s->gpr[rd] = getConditionCode(s, cc) ? s->gpr[rd] : s->gpr[rs];
    HISTO(s, mipsInsn::MOVF);
  }
  else {
    /* movt */
    s->gpr[rd] = getConditionCode(s, cc) ? s->gpr[rs] : s->gpr[rd];
    HISTO(s, mipsInsn::MOVT);    
  }
  s->pc += 4;
}

static void _cvts(uint32_t inst, state_t *s) {
  uint32_t fmt = (inst >> 21) & 31;
  uint32_t fd = (inst>>6) & 31;
  uint32_t fs = (inst>>11) & 31;
  if(currFpMode != fpMode::mips3) {
    assert((fd & 1) == 0);
    assert((fs & 1) == 0);
  }
  switch(fmt)
    {
    case FMT_D:
      *((float*)(s->cpr1 + fd)) = (float)(*((double*)(s->cpr1 + fs)));
      if(currFpMode != fpMode::mips3) {
	s->cpr1[fd+1] = 0;
      }
      s->cpr1_state[fd] = fp_reg_state::sp;      
      break;
    case FMT_W:
      *((float*)(s->cpr1 + fd)) = (float)(*((int32_t*)(s->cpr1 + fs)));
      if(currFpMode != fpMode::mips3) {
	*((float*)(s->cpr1 + fd + 1)) = 0;
      }
      break;
    case FMT_L:    /* cvt.s.l: 64-bit fixed -> single */
      *((float*)(s->cpr1 + fd)) = (float)(*((int64_t*)(s->cpr1 + fs)));
      if(currFpMode != fpMode::mips3) {
	*((float*)(s->cpr1 + fd + 1)) = 0;
      }
      break;
    default:
      printf("%s @ %d: unhandled fmt=%u inst=%08x pc=%08x\n", __func__, __LINE__, fmt, inst, (uint32_t)s->pc);
      exit(-1);
      break;
    }
  s->pc += 4;
}

static void _cvtd(uint32_t inst, state_t *s) {
  uint32_t fmt = (inst >> 21) & 31;
  uint32_t fd = (inst>>6) & 31;
  uint32_t fs = (inst>>11) & 31;
  switch(fmt)
    {
    case FMT_S:
      *((double*)(s->cpr1 + fd)) = (double)(*((float*)(s->cpr1 + fs)));
      s->cpr1_state[fd] = fp_reg_state::dp;
      break;
    case FMT_W:
     *((double*)(s->cpr1 + fd)) = (double)(*((int32_t*)(s->cpr1 + fs)));
      break;
    case FMT_L:    /* cvt.d.l: 64-bit fixed -> double */
      *((double*)(s->cpr1 + fd)) = (double)(*((int64_t*)(s->cpr1 + fs)));
      s->cpr1_state[fd] = fp_reg_state::dp;
      break;
    default:
      printf("%s @ %d: unhandled fmt=%u inst=%08x pc=%08x\n", __func__, __LINE__, fmt, inst, (uint32_t)s->pc);
      exit(-1);
      break;
    }
  s->pc += 4;
}

static void _fmovn(uint32_t inst, state_t *s) {
  uint32_t fmt = (inst >> 21) & 31;
  switch(fmt)
    {
    case FMT_S:
      _movns(inst, s);
      break;
    case FMT_D:
      _movnd(inst, s);
      break;
    default:
      printf("unsupported %s\n", __func__);
      exit(-1);
      break;
    }
}


static void _fmovz(uint32_t inst, state_t *s) {
  uint32_t fmt = (inst >> 21) & 31;
  switch(fmt)
    {
    case FMT_S:
      _movzs(inst, s);
      break;
    case FMT_D:
      _movzd(inst, s);
      break;
    default:
      printf("unsupported %s\n", __func__);
      exit(-1);
      break;
    }
}

static void _fmovc(uint32_t inst, state_t *s) {
  uint32_t fmt = (inst >> 21) & 31;
  switch(fmt)
    {
    case FMT_S:
      _movcs(inst, s);
      break;
    case FMT_D:
      _movcd(inst, s);
      break;
    default:
      printf("unsupported %s\n", __func__);
      exit(-1);
      break;
    }
}



template <typename T>
static void fpCmp(uint32_t inst, state_t *s) {
  uint32_t cond = inst & 15;
  uint32_t cc = (inst >> 8) & 7;
  uint32_t ft = (inst >> 16) & 31;
  uint32_t fs = (inst >> 11) & 31;
  T Tfs = *((T*)(s->cpr1+fs));
  T Tft = *((T*)(s->cpr1+ft));
  uint32_t v = 0;

  switch(cond)
    {
    case COND_UN:
      v = (Tfs == Tft);
      s->fcr1[CP1_CR25] = setBit(s->fcr1[CP1_CR25],v,cc);
      break;
    case COND_EQ:
      v = (Tfs == Tft);
      HISTO(s, select_fp_insn<T>(mipsInsn::DP_CMP_EQ, mipsInsn::SP_CMP_EQ));            
      s->fcr1[CP1_CR25] = setBit(s->fcr1[CP1_CR25],v,cc);
      break;
    case COND_LT:
      v = (Tfs < Tft);
      HISTO(s, select_fp_insn<T>(mipsInsn::DP_CMP_LT, mipsInsn::SP_CMP_LT));      
      s->fcr1[CP1_CR25] = setBit(s->fcr1[CP1_CR25],v,cc);
      break;
    case COND_LE:
      v = (Tfs <= Tft);
      HISTO(s, select_fp_insn<T>(mipsInsn::DP_CMP_LE, mipsInsn::SP_CMP_LE));            
      s->fcr1[CP1_CR25] = setBit(s->fcr1[CP1_CR25],v,cc);
      break;
    default:
      printf("unimplemented %s = %s\n", __func__, getCondName(cond).c_str());
      exit(-1);
      break;
    }
  if(globals::trace_retirement) {
    std::cout << std::hex
	      << s->pc
	      << std::dec
	      << " c. "
	      << Tfs
	      << " "
	      << getCondName(cond)
	      << " "
	      << Tft
	      << " = "
	      << v
	      << "\n";
  }
  
  s->pc += 4;
}

static void _c(uint32_t inst, state_t *s) {
  uint32_t fmt = (inst >> 21) & 31;
  switch(fmt)
    {
    case FMT_S:
      fpCmp<float>(inst,s);
      break;
    case FMT_D:
      fpCmp<double>(inst,s);
      break;
    default:
      printf("unsupported comparison\n");
      exit(-1);
      break;
    }
}

template< typename T, fpOperation op>
static void execFP(uint32_t inst, state_t *s) {
  uint32_t ft = (inst>>16)&31, fs=(inst>>11)&31, fd=(inst>>6)&31;
  T _fs = *reinterpret_cast<T*>(s->cpr1+fs);
  T _ft = *reinterpret_cast<T*>(s->cpr1+ft);
  T &_fd = *reinterpret_cast<T*>(s->cpr1+fd);

  switch(op)
    {
    case fpOperation::abs:
      _fd = std::abs(_fs);
      HISTO(s, select_fp_insn<T>(mipsInsn::DP_ABS, mipsInsn::SP_ABS));      
      break;
    case fpOperation::neg:
      _fd = -_fs;
      HISTO(s, select_fp_insn<T>(mipsInsn::DP_NEG, mipsInsn::SP_NEG));
      break;
    case fpOperation::mov:
      _fd = _fs;
      HISTO(s, select_fp_insn<T>(mipsInsn::DP_MOV, mipsInsn::SP_MOV));            
      break;
    case fpOperation::add:
      _fd = _fs + _ft;
      HISTO(s, select_fp_insn<T>(mipsInsn::DP_ADD, mipsInsn::SP_ADD));            
      break;
    case fpOperation::sub:
      _fd = _fs - _ft;
      HISTO(s, select_fp_insn<T>(mipsInsn::DP_SUB, mipsInsn::SP_SUB));                  
      break;
    case fpOperation::mul:
      _fd = _fs * _ft;
      HISTO(s, select_fp_insn<T>(mipsInsn::DP_MUL, mipsInsn::SP_MUL));      
      break;
    case fpOperation::div:
      if(_ft==0.0) {
	_fd = std::numeric_limits<T>::max();
      }
      else {
	_fd = _fs / _ft;
      }
      HISTO(s, select_fp_insn<T>(mipsInsn::DP_DIV, mipsInsn::SP_DIV));       
      break;
    case fpOperation::sqrt:
      _fd = std::sqrt(_fs);
      HISTO(s, select_fp_insn<T>(mipsInsn::DP_SQRT, mipsInsn::SP_SQRT));      
      break;
    case fpOperation::rsqrt:
      _fd = static_cast<T>(1.0) / std::sqrt(_fs);
      HISTO(s, select_fp_insn<T>(mipsInsn::DP_RSQRT, mipsInsn::SP_RSQRT));
      break;
    case fpOperation::recip:
      _fd = static_cast<T>(1.0) / _fs;
      HISTO(s, select_fp_insn<T>(mipsInsn::DP_RECIP, mipsInsn::SP_RECIP));
      break;
    default:
      UNREACHABLE();
    }
  s->pc+=4;
}

template <fpOperation op>
void do_fp_op(uint32_t inst, state_t *s) {
  int fd=(inst>>6)&31;
  switch((inst>>21)&31) {
  case FMT_S:
    execFP<float,op>(inst,s);
    s->cpr1_state[fd] = fp_reg_state::sp;
    if(currFpMode != fpMode::mips3) {   /* FR=0 pairing: even-only, scrub the high reg */
      assert((fd&1) == 0);
      s->cpr1[fd+1] = 0;
      s->cpr1_state[fd+1] = fp_reg_state::unknown;
    }
    break;
  case FMT_D:
    execFP<double,op>(inst,s);
    s->cpr1_state[fd] = fp_reg_state::dp;
    break;
  default:
    UNREACHABLE();
  }
}


template <bool EL>
static void execCoproc1(uint32_t inst, state_t *s) {
  uint32_t opcode = inst>>26;
  uint32_t functField = (inst>>21) & 31;
  uint32_t lowop = inst & 63;  
  uint32_t fmt = (inst >> 21) & 31;
  uint32_t nd_tf = (inst>>16) & 3;
  
  uint32_t lowbits = inst & ((1<<11)-1);
  opcode &= 0x3;

  if(fmt == 0x8)
    {
      switch(nd_tf)
	{
	case 0x0:
	  branch<EL,branch_type::bc1f>(inst, s);
	  break;
	case 0x1:
	  branch<EL,branch_type::bc1t>(inst, s);
	  break;
	case 0x2:
	  branch<EL,branch_type::bc1fl>(inst, s);
	  break;
	case 0x3:
	  branch<EL,branch_type::bc1tl>(inst, s);
	  break;
	}
      /*BRANCH*/
    }
  else if((lowbits == 0) && ((functField==0x0) || (functField==0x4) ||
			     (functField==0x2) || (functField==0x6) ||
			     (functField==0x1) || (functField==0x5)))
    {
      if(functField == 0x0)
	{
	  /* move from coprocessor */
	  _mfc1(inst,s);
	}
      else if(functField == 0x4)
	{
	  /* move to coprocessor */
	  _mtc1(inst,s);
	}
      else if(functField == 0x1)
	{
	  /* doubleword move from coprocessor (dmfc1) */
	  _dmfc1(inst,s);
	}
      else if(functField == 0x5)
	{
	  /* doubleword move to coprocessor (dmtc1) */
	  _dmtc1(inst,s);
	}
      else if(functField == 0x2)
	{
	  /* move from control coprocessor (cfc1) */
	  _cfc1(inst,s);
	}
      else if(functField == 0x6)
	{
	  /* move to control coprocessor (ctc1) */
	  _ctc1(inst,s);
	}
    }
  else
    {
      if((lowop >> 4) == 3)
	{
	  _c(inst, s);
	}
      else{
	switch(lowop)
	  {
	  case 0x0:
	    do_fp_op<fpOperation::add>(inst, s);
	    break;
	  case 0x1:
	    do_fp_op<fpOperation::sub>(inst, s);
	    break;
	  case 0x2:
	    do_fp_op<fpOperation::mul>(inst, s);
	    break;
	  case 0x3:
	    do_fp_op<fpOperation::div>(inst, s);
	    break;
	  case 0x4:
	    do_fp_op<fpOperation::sqrt>(inst, s);
	    break;
	  case 0x5:
	    do_fp_op<fpOperation::abs>(inst, s);
	    break;
	  case 0x6:
	    do_fp_op<fpOperation::mov>(inst, s);
	    break;
	  case 0x7:
	    do_fp_op<fpOperation::neg>(inst, s);
	    break;
	  case 0x9:
	    _truncl(inst, s);
	    break;
	  case 0xd:
	    _truncw(inst, s);
	    break;
	  case 0x11:
	    _fmovc(inst, s);
	    break;
	  case 0x12:
	    _fmovz(inst, s);
	    break;
	  case 0x13:
	    _fmovn(inst, s);
	    break;
	  case 0x15:
	    do_fp_op<fpOperation::recip>(inst, s);
	    break;
	  case 0x16:
	    do_fp_op<fpOperation::rsqrt>(inst, s);
	    break;
	  case 0x20:
	    /* cvt.s */
	    _cvts(inst, s);
	    break;
	  case 0x21:
	    _cvtd(inst, s);
	    break;
	  default:
	    printf("unhandled coproc1 instruction (%x) @ %08x\n",
		   inst, s->pc);
	    exit(-1);
	    break;
	  }
      }
    }
}

template <bool EL>
bool is_store_insn(state_t *s) {
  sparse_mem &mem = s->mem;
  uint32_t inst = bswap<EL>(mem.get<uint32_t>(va2pa(s->pc)));
  uint32_t opcode = inst>>26;
  switch(opcode)
    {
    case 0x28: //_sb(inst, s); 
    case 0x29: //_sh<EL>(inst, s); 
    case 0x2a: //_swl<EL>(inst, s); 
    case 0x2B: //_sw<EL>(inst, s); 
    case 0x2e: //_swr<EL>(inst, s);
    case 0x39: //_swc1<EL>(inst, s);
    case 0x38: //_sc
    case 0x3D: //_sdc1<EL>(inst, s);
      return true;
    default:
      break;
    }
  return false;
}


bool is_store_insn(state_t *s) {
  return is_store_insn<false>(s);
}


template <bool EL>
void execMips(state_t *s) {
  sparse_mem &mem = s->mem;
  s->gpr[0] = 0;   /* MIPS $0 is hardwired to zero; e.g. `mflo $0` must not stick */
  s->tlb_fault = false;
  uint32_t ipa = va_translate(s, (uint64_t)s->pc, tlb_op::fetch);
  if(s->tlb_fault) return;   /* instruction-fetch TLB miss -> vectored */
  uint32_t inst = bswap<EL>(mem.get<uint32_t>(ipa));
  if(globals::trace_retirement and false) {
    std::cout << std::hex
	      << "cosim "
	      << s->pc << ","
	      << std::dec << " : "
	      << getAsmString(inst, s->pc) << "\n";
  }
  //std::cout << std::hex << s->pc << std::dec << " : "
  //<< getAsmString(inst, s->pc) << "\n";
  uint32_t opcode = inst>>26;
  bool isRType = (opcode==0);
  bool isJType = ((opcode>>1)==1);
  bool isCoproc0 = (opcode == 0x10);
  bool isCoproc1 = (opcode == 0x11);
  bool isCoproc1x = (opcode == 0x13);
  bool isCoproc2 = (opcode == 0x12);
  bool isSpecial2 = (opcode == 0x1c); 
  bool isSpecial3 = (opcode == 0x1f);
  bool isLoadLinked = (opcode == 0x30);
  bool isStoreCond = (opcode == 0x38);
  bool isLoadLinkedD = (opcode == 0x34);   /* lld */
  bool isStoreCondD = (opcode == 0x3c);    /* scd */
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  s->icnt++;
  if(globals::trace_retirement and globals::retire_log) {
    /* optional TRACEWIN=lo:hi icnt window so a huge boot can emit a small,
     * focused retire_trace (e.g. around a single XTLB refill) for mips-analyzer. */
    static bool tw_init = false; static uint64_t tw_lo = 0, tw_hi = ~0ULL;
    if(!tw_init) { tw_init = true; const char *tw = getenv("TRACEWIN");
                   if(tw) sscanf(tw, "%lu:%lu", &tw_lo, &tw_hi); }
    if(s->icnt >= tw_lo && s->icnt < tw_hi)
      globals::retire_log->get_records().emplace_back(ipa, (uint64_t)s->pc, inst);
  }
  if(globals::pctrace) {
    if(!globals::pctrace_on && (uint32_t)s->pc == globals::pctrace_start) globals::pctrace_on = true;
    if(globals::pctrace_on) fprintf(globals::pctrace, "%08x\n", (uint32_t)s->pc);
  }
  {
    /* CALLWIN=lo:hi -- log every jal/jalr (caller pc -> callee target) in the
     * icnt window, for diffing the kernel call sequence against MAME. */
    static const char *cw = getenv("CALLWIN");
    static uint64_t cwlo = 0, cwhi = 0; static bool cwp = false;
    if(cw && !cwp) { sscanf(cw, "%llu:%llu", (unsigned long long*)&cwlo, (unsigned long long*)&cwhi); cwp = true; }
    if(cw && s->icnt >= cwlo && s->icnt < cwhi) {
      uint32_t op = inst >> 26;
      if(op == 3) /* jal */
        fprintf(stderr, "[call] %lu %08x %08x\n", (unsigned long)s->icnt,
                (uint32_t)s->pc, (uint32_t)((s->pc & ~0xfffffffULL) | ((uint64_t)(inst & 0x3ffffff) << 2)));
      else if(op == 0 && (inst & 0x3f) == 9) /* jalr */
        fprintf(stderr, "[call] %lu %08x %08x\n", (unsigned long)s->icnt,
                (uint32_t)s->pc, (uint32_t)s->gpr[(inst >> 21) & 31]);
    }
  }
  static const bool romvec_dbg = getenv("ROMVEC") != nullptr;
  if(romvec_dbg) {
    /* Log every control transfer into the ARCS romvec stub blob (PA 0x1000..
     * 0x1fff) -- jal/jalr/j/jr -- with args, so we can ask MAME what the real
     * PROM returns for each. (call_prom uses jr tail-calls, not just jalr.) */
    uint32_t op = inst >> 26, fn = inst & 0x3f, tgt = 0;
    bool xfer = false;
    if(op == 2 || op == 3) { tgt = (uint32_t)((s->pc & ~0xfffffffULL) | ((uint64_t)(inst & 0x3ffffff) << 2)); xfer = true; }
    else if(op == 0 && (fn == 8 || fn == 9)) { tgt = (uint32_t)s->gpr[(inst >> 21) & 31]; xfer = true; }
    if(xfer && (tgt & 0x1fffffff) >= 0x1000 && (tgt & 0x1fffffff) < 0x2000)
      fprintf(stderr, "[romvec] icnt=%lu caller=%08x target=%08x a0=%016lx a1=%016lx a2=%016lx a3=%016lx ra=%08x\n",
              (unsigned long)s->icnt, (uint32_t)s->pc, tgt,
              (long)s->gpr[4], (long)s->gpr[5], (long)s->gpr[6], (long)s->gpr[7], (uint32_t)s->gpr[31]);
  }
  static const bool kmissdbg = getenv("KMISSDBG") != nullptr;
  if((uint32_t)s->pc == 0x880162c8u && kmissdbg) {
    /* kmiss entry: for a fault in the KPTEBASE self-map region [0xff800000,
     * 0xffa00000), replicate the handler's PDA walk and dump whether the
     * page-table-of-page-table root is backed. PDA base ptr lives at kseg3
     * 0xffffa014; +0x378/+0x37c = two PT roots; +0x380 = entry limit. A zero
     * entry0/entry1 here is exactly the "0xff800000 never gets backed" panic. */
    uint32_t bv = (uint32_t)s->cpr0[CPR0_BADVADDR];
    static int n = 0;
    if(bv >= 0xff800000u && bv < 0xffa00000u && n++ < 8) {
      auto rd32 = [&](uint64_t va, bool &ok) -> uint32_t {
        uint32_t pa;
        if(tlb_probe_ro(s, va, &pa)) { ok = true; return bswap<EL>(s->mem.get<uint32_t>(pa)); }
        ok = false; return 0;
      };
      bool ok_pcb, ok_lim, ok_b0, ok_b1, ok_e0, ok_e1;
      uint32_t idx = (bv - 0xff800000u) >> 12;
      uint32_t pcb   = rd32(0xffffffffffffa014ULL, ok_pcb);   /* PDA base ptr  */
      uint32_t lim   = rd32((uint64_t)(int64_t)(int32_t)(pcb + 0x380), ok_lim);
      uint32_t base0 = rd32((uint64_t)(int64_t)(int32_t)(pcb + 0x378), ok_b0);
      uint32_t base1 = rd32((uint64_t)(int64_t)(int32_t)(pcb + 0x37c), ok_b1);
      uint32_t e0    = rd32((uint64_t)(int64_t)(int32_t)(base0 + idx*4), ok_e0);
      uint32_t e1    = rd32((uint64_t)(int64_t)(int32_t)(base1 + idx*4), ok_e1);
      bool ok_kpt, ok_klim;
      uint32_t kptbl = rd32(0xffffffff8832cf58ULL, ok_kpt);   /* global kernel PT base */
      uint32_t klim  = rd32(0xffffffff8832b588ULL, ok_klim);  /* kmissnxt limit */
      fprintf(stderr, "[kmiss] icnt=%lu bva=%08x idx=%u pcb=%08x(%d) lim=%08x(%d) "
              "base0=%08x(%d) e0=%08x(%d) base1=%08x(%d) e1=%08x(%d) "
              "kptbl=%08x(%d) klim=%08x(%d)\n",
              (unsigned long)s->icnt, bv, idx, pcb, ok_pcb, lim, ok_lim,
              base0, ok_b0, e0, ok_e0, base1, ok_b1, e1, ok_e1,
              kptbl, ok_kpt, klim, ok_klim);
    }
  }
  if((uint32_t)s->pc == 0x880052a4u && kmissdbg) {   /* resume entry */
    static unsigned long rc = 0;
    if(rc < 4 || (rc % 1000) == 0)
      fprintf(stderr, "[resume] #%lu icnt=%lu a0(thread)=%08x\n",
              rc, (unsigned long)s->icnt, (uint32_t)s->gpr[4]);
    rc++;
  }
  {
    /* KMISSTRACE: once the c0000000 bzero fires, log every TLB-handler entry so we
     * can see the routing for the 0xffb00000 page-table-region fault — does it reach
     * kmissnxt (global) or fall into kmiss's per-process longway? */
    static const bool kt = getenv("KMISSTRACE") != nullptr;
    static bool armed = false;
    if(kt) {
      if((uint32_t)s->pc == 0x8801a860u && (uint32_t)s->gpr[4] == 0xc0000000u) armed = true;
      if(armed) {
        uint32_t pc = (uint32_t)s->pc;
        const char *nm = pc==0x80000000u ? "refill_vec" : pc==0x80000180u ? "general_vec"
                       : pc==0x880162c8u ? "kmiss"      : pc==0x880164fcu ? "kmissnxt"
                       : pc==0x880165d4u ? "kmissnxt_c0" : pc==0x88015c10u ? "longway"
                       : pc==0x88016498u ? "kmiss_pathB" : nullptr;
        if(nm)
          fprintf(stderr, "[ktrace] icnt=%lu %-11s pc=%08x bva=%08x ctx=%08x exl=%d epc=%08x\n",
                  (unsigned long)s->icnt, nm, pc, s->cpr0[CPR0_BADVADDR], s->cpr0[CPR0_CONTEXT],
                  (s->cpr0[CPR0_SR] & SR_EXL) ? 1 : 0, s->cpr0[CPR0_EPC]);
      }
    }
  }
  static const bool kvaldbg = getenv("KVALDBG") != nullptr;
  if(kvaldbg) {
    /* kvalloc PTE-write loop: 0x880fd07c = `sw a3,-4(s0)` (kvalloc+0x2dc), the PC
     * MAME says writes the VALID leaf PTE into kptbl[c0000000] @ PA 0x08392000
     * (value 0x4020f61f) before the bzero. s0=$16 (post-incr -> slot is s0-4),
     * a3=$7 (PTE value). We log writes landing in the kptbl arena, plus read back
     * kptbl[c0000000] at the bzero fault (pc 0x8801a860) for direct comparison. */
    if((uint32_t)s->pc == 0x880fd07cu) {
      static unsigned long wn = 0;
      uint32_t va  = (uint32_t)s->gpr[16] - 4;     /* s0 - 4 */
      uint32_t pa  = va & 0x1fffffffu;             /* s0 is kseg0 */
      uint32_t val = (uint32_t)s->gpr[7];          /* a3 = PTE */
      bool in_arena = (pa >= 0x08392000u && pa < 0x08393000u);
      if(wn < 4 || in_arena)
        fprintf(stderr, "[kval] #%lu icnt=%lu pc=880fd07c va=%08x pa=%08x val=%08x%s\n",
                wn, (unsigned long)s->icnt, va, pa, val,
                pa == 0x08392000u ? "  <== kptbl[c0000000]" : "");
      wn++;
    }
    if((uint32_t)s->pc == 0x8801a860u && (uint32_t)s->gpr[4] == 0xc0000000u) {
      static int once = 0;                          /* the c0000000 bzero store */
      if(once++ < 2)
        fprintf(stderr, "[kval] bzero(c0000000)@8801a860 icnt=%lu kptbl[c0000000]@pa08392000=%08x\n",
                (unsigned long)s->icnt,
                bswap<EL>(s->mem.get<uint32_t>(0x08392000u)));
    }
  }

  /* 64-bit ops raise Reserved Instruction when not in 64-bit mode (matches the
   * RTL decode_mips.sv gate; the random instruction tests rely on this). */
  if(is_64b_gated(inst) && !in_64b_mode(s)) {
    take_exception_ri(s);
    return;
  }
    
  if(isRType) {
    uint32_t funct = inst & 63;
    uint32_t sa = (inst >> 6) & 31;
    switch(funct) 
      {
      case 0x00: /*sll*/
	s->gpr[rd] = static_cast<int32_t>(s->gpr[rt]) << sa;
	s->pc += 4;
	if(inst == 0) {
	  HISTO(s, mipsInsn::NOP);
	}
	else {
	  HISTO(s, mipsInsn::SLL);
	}
	break;
      case 0x01: /* movci */
	_movci(inst,s);
	break;
      case 0x02: /* srl */
	s->gpr[rd] = sext64(((uint32_t)s->gpr[rt] >> sa));
	s->pc += 4;
	HISTO(s, mipsInsn::SRL);
	break;
      case 0x03: /* sra */
	s->gpr[rd] = static_cast<int32_t>(s->gpr[rt]) >> sa;
	s->pc += 4;
	HISTO(s, mipsInsn::SRA);
	break;	
      case 0x04: /* sllv */
	s->gpr[rd] = sext64(static_cast<uint32_t>(s->gpr[rt]) << (s->gpr[rs] & 0x1f));
	s->pc += 4;
	HISTO(s, mipsInsn::SLLV);
	break;
      case 0x06:  /* srlv: sign-extend the 32-bit logical-shift result (MIPS64) */
	s->gpr[rd] = sext64((uint32_t)s->gpr[rt] >> (s->gpr[rs] & 0x1f));
	s->pc += 4;
	HISTO(s, mipsInsn::SRLV);
	break;
      case 0x07:  /* srav: 32-bit arithmetic shift, result sign-extended (MIPS64) */
	s->gpr[rd] = static_cast<int32_t>(s->gpr[rt]) >> (s->gpr[rs] & 0x1f);
	s->pc += 4;
	HISTO(s, mipsInsn::SRAV);
	break;
      case 0x08: { /* jr */
	state_t::reg_t jaddr = s->gpr[rs];
	s->pc += 4;
	if(!run_delay_slot<EL>(s))
	  s->pc = jaddr;
	HISTO(s, mipsInsn::JR);
	break;
      }
      case 0x09: { /* jalr */
	state_t::reg_t jaddr = s->gpr[rs];
	s->gpr[31] = s->pc + 8;   /* full 64-bit link (n64 user PCs exceed 32 bits) */
	s->pc += 4;
	if(!run_delay_slot<EL>(s))
	  s->pc = jaddr;
	HISTO(s, mipsInsn::JALR);
	break;
      }
      case 0x0C: /* syscall -> trap to kernel (ExcCode 8) */
	raise_syscall(s);
	HISTO(s, mipsInsn::BREAK);
	break;
      case 0x0D: /* break -> trap to kernel (ExcCode 9) */
	raise_break(s);
	HISTO(s, mipsInsn::BREAK);
	break;
      case 0x0f: /* sync */
	s->pc += 4;
	HISTO(s, mipsInsn::SYNC);
	break;
      case 0x10: /* mfhi */
	s->gpr[rd] = s->hi;
	s->pc += 4;
	HISTO(s, mipsInsn::MFHI);
	break;
      case 0x11: /* mthi */ 
	s->hi = s->gpr[rs];
	s->pc += 4;
	HISTO(s, mipsInsn::MTHI);
	break;
      case 0x12: /* mflo */
	s->gpr[rd] = s->lo;
	s->pc += 4;
	HISTO(s, mipsInsn::MFLO);	
	break;
      case 0x13: /* mtlo */
	s->lo = s->gpr[rs];
	s->pc += 4;
	HISTO(s, mipsInsn::MTLO);		
	break;
      case 0x18: { /* mult: 32x32 signed (operands are the low 32 bits) */
	int64_t y;
	y = (int64_t)(int32_t)s->gpr[rs] * (int64_t)(int32_t)s->gpr[rt];
	s->lo = (int32_t)(y & 0xffffffff);
	s->hi = (int32_t)(y >> 32);
	s->pc += 4;
	HISTO(s, mipsInsn::MULT);			
	break;
      }
      case 0x19: { /* multu */
	uint64_t y;
	uint64_t u0 = (uint64_t)(uint32_t)s->gpr[rs];
	uint64_t u1 = (uint64_t)(uint32_t)s->gpr[rt];
	y = u0*u1;
	s->lo = sext64((uint32_t)y);
	s->hi = sext64((uint32_t)(y>>32));
	s->pc += 4;
	HISTO(s, mipsInsn::MULTU);
	break;
      }
      case 0x1A: /* div: 32-bit signed, sign-extended (int64 avoids INT_MIN/-1 UB) */
	if((int32_t)s->gpr[rt] != 0) {
	  int64_t a = (int32_t)s->gpr[rs], b = (int32_t)s->gpr[rt];
	  s->lo = sext64((uint32_t)(int32_t)(a / b));
	  s->hi = sext64((uint32_t)(int32_t)(a % b));
	}
	s->pc += 4;
	HISTO(s, mipsInsn::DIV);
	break;
      case 0x1B: /* divu */
	if(s->gpr[rt] != 0) {
	  s->lo = sext64((uint32_t)s->gpr[rs] / (uint32_t)s->gpr[rt]);
	  s->hi = sext64((uint32_t)s->gpr[rs] % (uint32_t)s->gpr[rt]);
	}
	s->pc += 4;
	HISTO(s, mipsInsn::DIVU);
	break;
      case 0x1C: { /* dmult: signed 64x64 -> 128, hi:lo */
	__int128 y = (__int128)s->gpr[rs] * (__int128)s->gpr[rt];
	s->lo = (int64_t)(y & 0xffffffffffffffffULL);
	s->hi = (int64_t)(y >> 64);
	s->pc += 4;
	HISTO(s, mipsInsn::DMULT);
	break;
      }
      case 0x1D: { /* dmultu: unsigned 64x64 -> 128, hi:lo */
	unsigned __int128 y = (unsigned __int128)(uint64_t)s->gpr[rs]
	                    * (unsigned __int128)(uint64_t)s->gpr[rt];
	s->lo = (int64_t)(y & 0xffffffffffffffffULL);
	s->hi = (int64_t)(uint64_t)(y >> 64);
	s->pc += 4;
	HISTO(s, mipsInsn::DMULTU);
	break;
      }
      case 0x1E: /* ddiv: signed 64-bit divide */
	if(s->gpr[rt] != 0) {
	  s->lo = s->gpr[rs] / s->gpr[rt];
	  s->hi = s->gpr[rs] % s->gpr[rt];
	}
	s->pc += 4;
	HISTO(s, mipsInsn::DDIV);
	break;
      case 0x1F: /* ddivu: unsigned 64-bit divide */
	if(s->gpr[rt] != 0) {
	  s->lo = (int64_t)((uint64_t)s->gpr[rs] / (uint64_t)s->gpr[rt]);
	  s->hi = (int64_t)((uint64_t)s->gpr[rs] % (uint64_t)s->gpr[rt]);
	}
	s->pc += 4;
	HISTO(s, mipsInsn::DDIVU);
	break;
      case 0x20: { /* add */
	uint32_t u_rs = (uint32_t)s->gpr[rs];
	uint32_t u_rt = (uint32_t)s->gpr[rt];
	uint32_t result = u_rs + u_rt;
	/* Overflow iff same-sign inputs produce different-sign result.
	 * Matches RTL: w_add32_overflow = (result[31]!=rt[31]) & (rs[31]==rt[31]) */
	if (((result >> 31) != (u_rt >> 31)) && ((u_rs >> 31) == (u_rt >> 31))) {
	  raise_overflow(s);
	  break;
	}
	s->gpr[rd] = sext64(result);
	s->pc += 4;
	HISTO(s, mipsInsn::ADD);
	break;
      }
      case 0x21: { /* addu */
	uint32_t u_rs = (uint32_t)s->gpr[rs];
	uint32_t u_rt = (uint32_t)s->gpr[rt];
	s->gpr[rd] = sext64(u_rs + u_rt);
	s->pc += 4;
	HISTO(s, mipsInsn::ADDU);
	break;
      }
      case 0x22: { /* sub */
	uint32_t u_rs = (uint32_t)s->gpr[rs];
	uint32_t u_rt = (uint32_t)s->gpr[rt];
	uint32_t result = u_rs - u_rt;
	/* A-B overflows iff operands differ in sign AND result sign != rs (minuend).
	 * Matches RTL: w_sub32_overflow = (result[31]!=rs[31]) & (rs[31]!=rt[31]) */
	if (((result >> 31) != (u_rs >> 31)) && ((u_rs >> 31) != (u_rt >> 31))) {
	  raise_overflow(s);
	  break;
	}
	s->gpr[rd] = sext64(result);
	s->pc += 4;
	HISTO(s, mipsInsn::SUB);
	break;
      }
      case 0x23:{ /*subu*/  
	uint32_t u_rs = (uint32_t)s->gpr[rs];
	uint32_t u_rt = (uint32_t)s->gpr[rt];
	uint32_t y = u_rs - u_rt;
	s->gpr[rd] = sext64(y);
	s->pc += 4;
	HISTO(s, mipsInsn::SUBU);
	break;
      }
      case 0x24: /* and */
	s->gpr[rd] = s->gpr[rs] & s->gpr[rt];
	s->pc += 4;
	HISTO(s, mipsInsn::AND);
	break;
      case 0x25: /* or */
	if(rd != 0) {
	  s->gpr[rd] = s->gpr[rs] | s->gpr[rt];
	}
	s->pc += 4;
	HISTO(s, mipsInsn::OR);
	break;
      case 0x26: /* xor */
	s->gpr[rd] = s->gpr[rs] ^ s->gpr[rt];
	s->pc += 4;
	HISTO(s, mipsInsn::XOR);	
	break;
      case 0x27: /* nor */
	s->gpr[rd] = ~(s->gpr[rs] | s->gpr[rt]);
	s->pc += 4;
	HISTO(s, mipsInsn::NOR);
	break;
      case 0x2A: /* slt */
	s->gpr[rd] = s->gpr[rs] < s->gpr[rt];
	s->pc += 4;
	HISTO(s, mipsInsn::SLT);	
	break;
      case 0x2B: { /* sltu */
	s->gpr[rd] = ((uint64_t)s->gpr[rs] < (uint64_t)s->gpr[rt]);
	s->pc += 4;
	HISTO(s, mipsInsn::SLTU);
	break;
      }
      case 0x0B: /* movn */
	s->gpr[rd] = (s->gpr[rt] != 0) ? s->gpr[rs] : s->gpr[rd];
	s->pc +=4;
	HISTO(s, mipsInsn::MOVN);
	break;
      case 0x0A: /* movz */
	s->gpr[rd] = (s->gpr[rt] == 0) ? s->gpr[rs] : s->gpr[rd];
	s->pc += 4;
	HISTO(s, mipsInsn::MOVZ);	
	break;
      case 0x30: /* tge  */
	if((int64_t)s->gpr[rs] >= (int64_t)s->gpr[rt]) { raise_trap(s); return; }
	s->pc += 4; HISTO(s, mipsInsn::TGE); break;
      case 0x31: /* tgeu */
	if((uint64_t)s->gpr[rs] >= (uint64_t)s->gpr[rt]) { raise_trap(s); return; }
	s->pc += 4; HISTO(s, mipsInsn::TGEU); break;
      case 0x32: /* tlt  */
	if((int64_t)s->gpr[rs] < (int64_t)s->gpr[rt]) { raise_trap(s); return; }
	s->pc += 4; HISTO(s, mipsInsn::TLT); break;
      case 0x33: /* tltu */
	if((uint64_t)s->gpr[rs] < (uint64_t)s->gpr[rt]) { raise_trap(s); return; }
	s->pc += 4; HISTO(s, mipsInsn::TLTU); break;
      case 0x34: /* teq */
	if(s->gpr[rs] == s->gpr[rt]) {
	  raise_trap(s);
	  return;
	}
	s->pc += 4;
	HISTO(s, mipsInsn::TEQ);
	break;
      case 0x36: /* tne */
	if(s->gpr[rs] != s->gpr[rt]) {
	  raise_trap(s);
	  return;
	}
	s->pc += 4;
	HISTO(s, mipsInsn::TNE);
	break;
      case 0x2C: { /* dadd */
	uint64_t u_rs = (uint64_t)s->gpr[rs];
	uint64_t u_rt = (uint64_t)s->gpr[rt];
	uint64_t result = u_rs + u_rt;
	/* Matches RTL: w_add64_overflow = (result[63]!=rt[63]) & (rs[63]==rt[63]) */
	if (((result >> 63) != (u_rt >> 63)) && ((u_rs >> 63) == (u_rt >> 63))) {
	  raise_overflow(s);
	  break;
	}
	s->gpr[rd] = (int64_t)result;
	s->pc += 4;
	HISTO(s, mipsInsn::DADD);
	break;
      }
      case 0x2D: /* daddu */
	s->gpr[rd] = s->gpr[rs] + s->gpr[rt];
	s->pc += 4;
	HISTO(s, mipsInsn::DADDU);
	break;
      case 0x2E: { /* dsub */
	uint64_t u_rs = (uint64_t)s->gpr[rs];
	uint64_t u_rt = (uint64_t)s->gpr[rt];
	uint64_t result = u_rs - u_rt;
	/* A-B overflows iff operands differ in sign AND result sign != rs (minuend).
	 * Matches RTL: w_sub64_overflow = (result[63]!=rs[63]) & (rs[63]!=rt[63]) */
	if (((result >> 63) != (u_rs >> 63)) && ((u_rs >> 63) != (u_rt >> 63))) {
	  raise_overflow(s);
	  break;
	}
	s->gpr[rd] = (int64_t)result;
	s->pc += 4;
	HISTO(s, mipsInsn::DSUB);
	break;
      }
      case 0x2F: /* dsubu */
	s->gpr[rd] = s->gpr[rs] - s->gpr[rt];
	s->pc += 4;
	HISTO(s, mipsInsn::DSUBU);
	break;
      case 0x14: /* dsllv: rd = rt << rs[5:0] */
	s->gpr[rd] = s->gpr[rt] << (s->gpr[rs] & 63);
	s->pc += 4;
	HISTO(s, mipsInsn::DSLLV);
	break;
      case 0x16: /* dsrlv: rd = rt >> rs[5:0] (logical) */
	s->gpr[rd] = (int64_t)((uint64_t)s->gpr[rt] >> (s->gpr[rs] & 63));
	s->pc += 4;
	HISTO(s, mipsInsn::DSRLV);
	break;
      case 0x17: /* dsrav: rd = rt >> rs[5:0] (arithmetic) */
	s->gpr[rd] = s->gpr[rt] >> (s->gpr[rs] & 63);
	s->pc += 4;
	HISTO(s, mipsInsn::DSRAV);
	break;
      case 0x38: /* dsll: rd = rt << sa */
	s->gpr[rd] = s->gpr[rt] << sa;
	s->pc += 4;
	HISTO(s, mipsInsn::DSLL);
	break;
      case 0x3A: /* dsrl: rd = rt >> sa (logical) */
	s->gpr[rd] = (int64_t)((uint64_t)s->gpr[rt] >> sa);
	s->pc += 4;
	HISTO(s, mipsInsn::DSRL);
	break;
      case 0x3B: /* dsra: rd = rt >> sa (arithmetic) */
	s->gpr[rd] = s->gpr[rt] >> sa;
	s->pc += 4;
	HISTO(s, mipsInsn::DSRA);
	break;
      case 0x3C: /* dsll32: rd = rt << (sa + 32) */
	s->gpr[rd] = s->gpr[rt] << (sa + 32);
	s->pc += 4;
	HISTO(s, mipsInsn::DSLL32);
	break;
      case 0x3E: /* dsrl32: rd = rt >> (sa + 32) (logical) */
	s->gpr[rd] = (int64_t)((uint64_t)s->gpr[rt] >> (sa + 32));
	s->pc += 4;
	HISTO(s, mipsInsn::DSRL32);
	break;
      case 0x3F: /* dsra32: rd = rt >> (sa + 32) (arithmetic) */
	s->gpr[rd] = s->gpr[rt] >> (sa + 32);
	s->pc += 4;
	HISTO(s, mipsInsn::DSRA32);
	break;
      default:
	raise_ri(s, inst);
	break;
      }
  }
  else if(isSpecial2 || isSpecial3) {
    /* RDHWR (SPECIAL3, funct 0x3b) is not in MIPS-III; Linux emulates it via the
     * RI handler (simulate_rdhwr) for the TLS thread pointer etc.  Raise RI
     * SILENTLY (matches r9999: unknown SPECIAL3 -> II -> ResI) so the guest
     * kernel emulates it without spamming the "unimplemented" diagnostic. */
    if(isSpecial3 && (inst & 0x3fu) == 0x3b)
      take_exception_ri(s);
    else
      raise_ri(s, inst);   /* MIPS32 SPECIAL2/SPECIAL3 are not in MIPS-III (R4000) */
  }
  else if(isJType) {
    state_t::reg_t jaddr = inst & ((1<<26)-1);
    jaddr <<= 2;
    if(opcode==0x2) { /* j */
      s->pc += 4;
      HISTO(s, mipsInsn::J);
    }
    else if(opcode==0x3) { /* jal */
      s->gpr[31] = s->pc + 8;   /* full 64-bit link (n64 user PCs exceed 32 bits) */
      s->pc += 4;
      HISTO(s, mipsInsn::JAL);
    }
    else {
      printf("Unknown JType instruction\n");
      exit(-1);
    }
    jaddr |= (s->pc & (~static_cast<state_t::reg_t>((1<<28)-1)));
    if(!run_delay_slot<EL>(s))
      s->pc = jaddr;
    //printf("new pc = %lx\n", jaddr);
  }
  else if(isCoproc0) {
    if( ((inst >> 25)&1) ) {
      /* CO=1 instructions: TLB ops, ERET, WAIT */
      switch(inst & 63)
	{
	case 0x1: { /* TLBR -- read TLB[Index] into staging regs */
	  uint32_t idx = s->cpr0[CPR0_INDEX] & 63;
	  if(idx < (uint32_t)state_t::NUM_TLB_ENTRIES) {
	    s->cpr0_64[CPR0_ENTRYHI]  = s->tlb[idx].entry_hi;
	    s->cpr0_64[CPR0_ENTRYLO0] = s->tlb[idx].entry_lo0;
	    s->cpr0_64[CPR0_ENTRYLO1] = s->tlb[idx].entry_lo1;
	    s->cpr0[CPR0_ENTRYHI]     = (uint32_t)s->tlb[idx].entry_hi;
	    s->cpr0[CPR0_ENTRYLO0]    = (uint32_t)s->tlb[idx].entry_lo0;
	    s->cpr0[CPR0_ENTRYLO1]    = (uint32_t)s->tlb[idx].entry_lo1;
	    s->cpr0[CPR0_PAGEMASK]    = s->tlb[idx].page_mask;
	  }
	  HISTO(s, mipsInsn::TLBR);
	  break;
	}
	case 0x2: { /* TLBWI -- write staging regs to TLB[Index] */
	  uint32_t idx = s->cpr0[CPR0_INDEX] & 63;
	  if(idx < (uint32_t)state_t::NUM_TLB_ENTRIES) {
	    s->tlb[idx].entry_hi  = s->cpr0_64[CPR0_ENTRYHI];
	    s->tlb[idx].entry_lo0 = s->cpr0_64[CPR0_ENTRYLO0];
	    s->tlb[idx].entry_lo1 = s->cpr0_64[CPR0_ENTRYLO1];
	    s->tlb[idx].page_mask = s->cpr0[CPR0_PAGEMASK];
	    assert(s->tlb[idx].page_mask == 0);
	  }
	  utlb_flush();   /* mapping changed -> drop the micro-TLB */
	  static const bool tlblog = getenv("TLBLOG") != nullptr;
	  if(tlblog) fprintf(stderr, "[TLBWI] idx=%2u hi=%016llx lo0=%016llx lo1=%016llx pm=%08x\n",
	     idx, (unsigned long long)s->cpr0_64[CPR0_ENTRYHI], (unsigned long long)s->cpr0_64[CPR0_ENTRYLO0],
	     (unsigned long long)s->cpr0_64[CPR0_ENTRYLO1], s->cpr0[CPR0_PAGEMASK]);
	  HISTO(s, mipsInsn::TLBWI);
	  break;
	}
	case 0x6: { /* TLBWR -- write staging regs to TLB[Random] */
	  uint32_t idx = s->cpr0[CPR0_RANDOM] & 63;
	  if(idx < (uint32_t)state_t::NUM_TLB_ENTRIES) {
	    s->tlb[idx].entry_hi  = s->cpr0_64[CPR0_ENTRYHI];
	    s->tlb[idx].entry_lo0 = s->cpr0_64[CPR0_ENTRYLO0];
	    s->tlb[idx].entry_lo1 = s->cpr0_64[CPR0_ENTRYLO1];
	    s->tlb[idx].page_mask = s->cpr0[CPR0_PAGEMASK];
	    assert(s->tlb[idx].page_mask == 0);	    
	  }
	  utlb_flush();   /* mapping changed -> drop the micro-TLB */
	  /* Decrement Random, wrap to NUM_TLB_ENTRIES-1 when it reaches Wired */
	  {
	    uint32_t wired  = s->cpr0[CPR0_WIRED] & 63;
	    uint32_t random = s->cpr0[CPR0_RANDOM] & 63;
	    s->cpr0[CPR0_RANDOM] = (random <= wired)
	      ? (uint32_t)(state_t::NUM_TLB_ENTRIES - 1) : (random - 1);
	  }
	  static const bool tlblog = getenv("TLBLOG") != nullptr;
	  if(tlblog && idx < 8) fprintf(stderr, "[TLBWR] idx=%2u hi=%016llx lo0=%016llx lo1=%016llx\n",
	     idx, (unsigned long long)s->cpr0_64[CPR0_ENTRYHI], (unsigned long long)s->cpr0_64[CPR0_ENTRYLO0],
	     (unsigned long long)s->cpr0_64[CPR0_ENTRYLO1]);
	  HISTO(s, mipsInsn::TLBWR);
	  break;
	}
	case 0x8: { /* TLBP -- probe TLB for matching entry */
	  uint64_t probe_hi   = s->cpr0_64[CPR0_ENTRYHI];
	  uint64_t probe_asid = probe_hi & 0xffu;
	  bool found = false;
	  for(int i = 0; i < state_t::NUM_TLB_ENTRIES; i++) {
	    /* Apply page-mask to get the significant VPN2 bits */
	    uint64_t mask    = ~(uint64_t)(s->tlb[i].page_mask | 0x1fffu);
	    bool global      = (s->tlb[i].entry_lo0 & 1u) &&
	                       (s->tlb[i].entry_lo1 & 1u);
	    bool vpn_match   = (probe_hi & mask) == (s->tlb[i].entry_hi & mask);
	    bool asid_match  = global ||
	                       (probe_asid == (s->tlb[i].entry_hi & 0xffu));
	    if(vpn_match && asid_match) {
	      s->cpr0[CPR0_INDEX] = (uint32_t)i; /* P=0, index=i */
	      found = true;
	      break;
	    }
	  }
	  if(!found) {
	    s->cpr0[CPR0_INDEX] |= (1u << 31); /* P=1 (probe failed) */
	  }
	  HISTO(s, mipsInsn::TLBP);
	  break;
	}
	case 24: { /* ERET -- exception return */
	  /* EPC/ErrorEPC are 64-bit on R4000: read the full cpr0_64 view, not the
	   * 32-bit cpr0[] (which truncates a user EPC like 0x120000190 -> 0x20000190,
	   * so eret never reaches userspace). Kernel ckseg0 EPCs sign-extend either
	   * way; userspace n64 EPCs need the full 64 bits. (-4 cancels the loop's
	   * post-instruction pc+=4.) */
	  if(s->cpr0[CPR0_SR] & SR_ERL) {
	    /* Return from error: EPC = ErrorEPC, clear ERL */
	    s->pc = (state_t::reg_t)s->cpr0_64[CPR0_ERROREPC] - 4;
	    s->cpr0[CPR0_SR] &= ~SR_ERL;
	  } else {
	    /* Return from exception: PC = EPC, clear EXL */
	    s->pc = (state_t::reg_t)s->cpr0_64[CPR0_EPC] - 4;
	    s->cpr0[CPR0_SR] &= ~SR_EXL;
	  }
	  HISTO(s, mipsInsn::ERET);
	  break;
	}
	case 32: //WAIT
	  if((s->cpr0[CPR0_SR] & 1) == 0) {
	    printf("attempting to wait with interrupts disabled @ VA %x, PA %x\n",
		   s->pc, VA2PA(s->pc));
	    exit(-1);
	  }
	  HISTO(s, mipsInsn::WAIT);
	  break;
	default:
	  exit(-1);
	}
    }
    else if( (((inst >> 21) & 31) == 11 ) &&
	     ((inst & 65535) == 0x6000) ) {
      //DI
      if(rt != 0) {
	s->gpr[rt] = s->cpr0[CPR0_SR];
      }
      s->cpr0[CPR0_SR] &= (~1U);
      HISTO(s, mipsInsn::DI);
    }
    else if( (((inst >> 21) & 31) == 11 ) &&
	     ((inst & 65535) == 0x6020) ) {
      //EI
      if(rt != 0) {
	s->gpr[rt] = s->cpr0[CPR0_SR];
      }
      s->cpr0[CPR0_SR] |= 1U;
      HISTO(s, mipsInsn::EI);
    }

    else {
      switch(rs)
	{
	case 0x0: /*mfc0*/
	  if(rd == 7) {
	    s->gpr[rt] = 0;
	  } else {
	    /* mfc0 sign-extends the 32-bit CP0 value to 64 bits, matching HW. */
	    s->gpr[rt] = sext32(s->cpr0[rd]);
	  }
	  HISTO(s, mipsInsn::MFC0);
	  break;
	case 0x1: /*dmfc0 -- read full 64-bit CP0 register */
	  s->gpr[rt] = s->cpr0_64[rd];
	  HISTO(s, mipsInsn::DMFC0);
	  break;
	case 0x4: /*mtc0*/
	  if(rd != 15) { /* PRId (reg 15) is read-only */
	    if(rd == CPR0_ENTRYHI) {
	      /* Sail mips_insts.sail execute(MTC0 ...EntryHi): take R[63:62],
	       * VPN2[39:13], ASID[7:0] from the FULL 64-bit GPR (no zero-extend;
	       * MTC0 behaves like DMTC0 for this register's fields). */
	      s->cpr0_64[rd] = (s->gpr[rt] & 0xc000000000000000ULL)   /* R    */
	                     | (s->gpr[rt] & 0x000000ffffffe000ULL)   /* VPN2 */
	                     | (s->gpr[rt] & 0xffULL);                /* ASID */
	    } else {
	      s->cpr0_64[rd] = (uint64_t)(uint32_t)s->gpr[rt];
	    }
	    s->cpr0[rd] = (uint32_t)s->cpr0_64[rd];
	  }
	  if(rd == CPR0_COMPARE)   /* writing Compare re-arms + clears the timer interrupt (IP[7]) */
	    s->cpr0[CPR0_CAUSE] &= ~(1u << 15);
	  /* CP0 reg 7 is the simulator putchar port */
	  if(rd == 7 && !s->silent) {
	    fputc((int)(s->gpr[rt] & 0xff), stdout);
	    fflush(stdout);
	  }
	  HISTO(s, mipsInsn::MTC0);
	  break;
	case 0x5: /*dmtc0 -- write full 64-bit CP0 register */
	  if(rd != 15) { /* PRId (reg 15) is read-only */
	    s->cpr0_64[rd] = s->gpr[rt];
	    s->cpr0[rd] = (uint32_t)s->gpr[rt];
	  }
	  if(rd == 7 && !s->silent) {
	    fputc((int)(s->gpr[rt] & 0xff), stdout);
	    fflush(stdout);
	  }
	  HISTO(s, mipsInsn::DMTC0);
	  break;
	default:
	  std::cerr << "unhandled cpr0 instruction @ "
		    << std::hex << s->pc << std::dec << "\n";
	  exit(-1);
	  break;
	}
      }
    s->pc += 4;
  }
  else if(isCoproc1) 
    execCoproc1<EL>(inst,s);
  else if(isCoproc1x)
    execCoproc1x<EL>(inst,s);
  else if(isCoproc2) {
    printf("coproc2 unimplemented\n");  exit(-1);
  }
  else if(isLoadLinked)
    _lw<EL>(inst, s);
  else if(isStoreCond)
    _sc<EL>(inst, s);
  else if(isLoadLinkedD)
    _ld<EL>(inst, s);
  else if(isStoreCondD)
    _scd<EL>(inst, s);
  else { /* itype */
    uint32_t uimm32 = inst & ((1<<16) - 1);
    int16_t simm16 = (int16_t)uimm32;
    int32_t simm32 = (int32_t)simm16;
    int32_t tmp;
    switch(opcode) 
      {
      case 0x01:
	_bgez_bltz<EL>(inst, s); 
	break;
      case 0x04:
	branch<EL,branch_type::beq>(inst, s);
	break;
      case 0x05:
	branch<EL,branch_type::bne>(inst, s); 
	break;
      case 0x06:
	branch<EL,branch_type::blez>(inst, s); 
	break;
      case 0x07:
	branch<EL,branch_type::bgtz>(inst, s); 
	break;
      case 0x08: /* addi */
	s->gpr[rt] = s->gpr[rs] + simm32;  
	s->pc+=4;
	HISTO(s, mipsInsn::ADDI);
	break;
      case 0x09: /* addiu: 32-bit add, result sign-extended (MIPS64) */
	tmp = sext64((uint32_t)(s->gpr[rs] + simm32));
	s->gpr[rt] = tmp;
	s->pc+=4;
	HISTO(s, mipsInsn::ADDIU);
	break;
      case 0x0A: /* slti */
	s->gpr[rt] = (s->gpr[rs] < simm32);
	s->pc += 4;
	HISTO(s, mipsInsn::SLTI);
	break;
      case 0x0B:/* sltiu */
	s->gpr[rt] = ((uint64_t)s->gpr[rs] < (uint64_t)(int64_t)simm32);
	s->pc += 4;
	HISTO(s, mipsInsn::SLTIU);
	break;
      case 0x0c: /* andi */
	s->gpr[rt] = s->gpr[rs] & uimm32;
	s->pc += 4;
	HISTO(s, mipsInsn::ANDI);
	break;
      case 0x0d: /* ori */
	s->gpr[rt] = s->gpr[rs] | uimm32;
	s->pc += 4;
	HISTO(s, mipsInsn::ORI);
	break;
      case 0x0e: /* xori */
	s->gpr[rt] = s->gpr[rs] ^ uimm32;
	s->pc += 4;
	HISTO(s, mipsInsn::XORI);
	break;
      case 0x0F: /* lui */
	uimm32 <<= 16;
	s->gpr[rt] = sext64(uimm32);
	s->pc += 4;
	HISTO(s, mipsInsn::LUI);
	break;
      case 0x14:
	branch<EL,branch_type::beql>(inst, s); 
	break;
      case 0x16:
	branch<EL,branch_type::blezl>(inst, s); 
	break;
      case 0x15:
	branch<EL,branch_type::bnel>(inst, s); 
	break;
      case 0x17:
	branch<EL,branch_type::bgtzl>(inst, s);
	break;
      case 0x18: { /* daddi: doubleword add immediate, traps on signed 64-bit overflow */
	uint64_t u_rs  = (uint64_t)s->gpr[rs];
	uint64_t u_imm = (uint64_t)(int64_t)simm32;   /* sign-extend imm to 64 bits */
	uint64_t result = u_rs + u_imm;
	/* same overflow rule as dadd (0x2C): same-sign inputs, different-sign result */
	if (((result >> 63) != (u_imm >> 63)) && ((u_rs >> 63) == (u_imm >> 63))) {
	  raise_overflow(s);
	  break;
	}
	s->gpr[rt] = (int64_t)result;
	s->pc += 4;
	HISTO(s, mipsInsn::DADDI);
	break;
      }
      case 0x19: /* daddiu */
	s->gpr[rt] = s->gpr[rs] + simm32;
	s->pc += 4;
	HISTO(s, mipsInsn::DADDIU);
	break;
      case 0x1a:
	_ldl<EL>(inst, s);
	break;
      case 0x1b:
	_ldr<EL>(inst, s);
	break;
      case 0x20:
	_lb(inst, s);
	break;
      case 0x21:
	_lh<EL>(inst, s);
	break;
      case 0x22: 
	_lwl<EL>(inst, s);
	break;
      case 0x23:
	_lw<EL>(inst, s);
	break;
      case 0x24:
	_lbu(inst, s);
	break;
      case 0x27: { /* lwu: load word unsigned (zero-extend to 64 bits) */
	uint32_t rs_ = (inst >> 21) & 31;
	uint32_t rt_ = (inst >> 16) & 31;
	int32_t imm  = (int32_t)(int16_t)(inst & 0xffffu);
	uint32_t ea  = va_translate(s, s->gpr[rs_] + imm, tlb_op::load);
	if(s->tlb_fault) break;
	if(ea & 3) { raise_adel(s); break; }
	s->gpr[rt_] = (uint64_t)(uint32_t)bswap<EL>(s->mem.get<int32_t>(ea));
	s->pc += 4;
	HISTO(s, mipsInsn::LWU);
	break;
      }
      case 0x25:
	_lhu<EL>(inst, s);
	break;
      case 0x26:
	_lwr<EL>(inst, s);
	break;
      case 0x28:
	_sb(inst, s); 
	break;
      case 0x29:
	_sh<EL>(inst, s); 
	break;
      case 0x2a:
	_swl<EL>(inst, s);
	break;
      case 0x2B:
	_sw<EL>(inst, s);
	break;
      case 0x2c:
	_sdl<EL>(inst, s);
	break;
      case 0x2d:
	_sdr<EL>(inst, s);
	break;
      case 0x2e:
	_swr<EL>(inst, s);
	break;
      case 0x2f: /* cache -- treated as NOP for now */
	s->pc += 4;
	break;
      case 0x31:
	_lwc1<EL>(inst, s);
	break;
      case 0x33: /* prefetch */
	s->pc += 4;
	break;
      case 0x35:
	_ldc1<EL>(inst, s);
	break;
      case 0x39:
	_swc1<EL>(inst, s);
	break;
      case 0x37:
	_ld<EL>(inst, s);
	break;
      case 0x3D:
	_sdc1<EL>(inst, s);
	break;
      case 0x3F:
	_sd<EL>(inst, s);
	break;
      default:
	raise_ri(s, inst);
	break;
      }
  }
}
