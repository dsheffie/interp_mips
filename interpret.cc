#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <unordered_map>
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

static const bool g_cache_dbg = getenv("CACHEDBG") != nullptr;

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
  { const char *pe = getenv("PRID"); uint32_t pv = pe ? (uint32_t)strtoull(pe,0,0) : PRID_VALUE;
    s->cpr0[CPR0_PRID] = pv; s->cpr0_64[CPR0_PRID] = pv; }
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
static const uint64_t INT_POLL = getenv("INT_POLL") ? strtoull(getenv("INT_POLL"),0,0) : 1;

/* env-gated TLB/timer tally (TLBTALLY=1): compare against the FPGA bad-istack trigger */
static uint64_t g_ty_timer=0, g_ty_tlbmiss=0, g_ty_idle_tlbmiss=0, g_ty_timer_in_tlbmiss=0, g_ty_timer_idle=0, g_ty_pda_miss=0;
static inline bool ty_in_tlbmiss(uint32_t pc){ return pc>=0x880196bcu && pc<=0x880196e0u; }
static inline bool ty_idle_sp(uint32_t sp){ return (sp & 0xfffff000u)==0x8834a000u; }
void maybe_take_interrupt(state_t *s) {
  { static const unsigned long g_pcs = getenv("PCSAMPLE") ? strtoul(getenv("PCSAMPLE"),0,0) : 0;
    if(g_pcs && (s->icnt % g_pcs) == 0)
      fprintf(stderr, "[pcsample] icnt=%llu pc=%08x\n", (unsigned long long)s->icnt, (uint32_t)s->pc); }
  if(s->scc) s->scc->tick(1);                          /* serial TX timing: keep exact */
  /* CP0 Count is architecturally visible and the kernel's delay/clock
   * calibration reads it, so advance it (and latch the timer IP7) every
   * instruction -- cheap, and keeps the timer cycle-accurate. */
  s->cpr0[CPR0_COUNT] = (s->cpr0[CPR0_COUNT] + 1u) & 0xffffffffu;
  if(s->cpr0[CPR0_COUNT] == s->cpr0[CPR0_COMPARE])
    s->cpr0[CPR0_CAUSE] |= (1u << 15);                 /* IP[7] = timer */

  /* SH_PROBE=<pc>: golden a3/a4/a0 at a /sbin/sh insn (compare vs the RTL fault:
   * RTL faulted sw a4,24(a3) @0x0e033b58 with a3=0x0e0a6ee8. a3 same+store OK -> TLB bug;
   * a3 different -> a3 register corruption in the RTL. */
  { static const bool g_cp = getenv("CRASHPROBE") != nullptr;
    if(g_cp && (uint32_t)s->pc == 0x0e6818b4u)   /* silicon crash store: sw s0,0(t1) */
      fprintf(stderr,"[crash] pc=0e6818b4 t1(addr)=%08x a0=%08x Status.FR=%u Status=%08x icnt=%llu\n",
        (uint32_t)s->gpr[9],(uint32_t)s->gpr[4],((s->cpr0[12]>>26)&1u),s->cpr0[12],(unsigned long long)s->icnt);
    /* golden FP capacity-sizer operands: at add.d f5,f6,f4 (0e636964) */
    if(g_cp && (uint32_t)s->pc == 0x0e636964u) {
      double f4=*((double*)(s->cpr1+4)), f6=*((double*)(s->cpr1+6));
      fprintf(stderr,"[capfp] add.d: f6(const)=%.17g f4=%.17g -> f6+f4=%.17g trunc=%d icnt=%llu\n",
        f6, f4, f6+f4, (int)(f6+f4), (unsigned long long)s->icnt);
    }
    /* golden FP grow intermediates: after mul.d f5,f4,f30 (0e774174) */
    if(g_cp && (uint32_t)s->pc == 0x0e77417cu) {
      double f4=*((double*)(s->cpr1+4)), f5=*((double*)(s->cpr1+5));
      double f30=*((double*)(s->cpr1+30)), f28=*((double*)(s->cpr1+28));
      fprintf(stderr,"[fp] t1(gpr9)=%d oldcap(t0)=%u f4=%g f30(ratio)=%g f5=t1xf30=%g f28(floor)=%g icnt=%llu\n",
        (int)(int32_t)s->gpr[9], (uint32_t)s->gpr[8], f4, f30, f5, f28, (unsigned long long)s->icnt);
    }
    /* golden reference at the array-insert grow-test (idx=a2, capacity=t0 both live) */
    if(g_cp && (uint32_t)s->pc == 0x0e774158u) {
      uint32_t idx=(uint32_t)s->gpr[6], cap=(uint32_t)s->gpr[8], s4=(uint32_t)s->gpr[20];
      fprintf(stderr,"[ref] s4=%08x idx=%u cap=%u %s tbl=%08x icnt=%llu\n",
        s4, idx, cap, (idx>cap?"OOB!":"ok"), (uint32_t)s->gpr[2], (unsigned long long)s->icnt);
    }
  }
  { static const bool g_bp = getenv("BASE_PROBE") != nullptr;
    if(g_bp && (uint32_t)s->pc == 0x0e7742b8u)
      { fprintf(stderr,"[GROW] base(v0)=%08x cap(t0)=%u cap(s7)=%u s5=%08x s4=%08x icnt=%llu\n",(uint32_t)s->gpr[2],(uint32_t)s->gpr[8],(uint32_t)s->gpr[23],(uint32_t)s->gpr[21],(uint32_t)s->gpr[20],(unsigned long long)s->icnt); }
    if(g_bp && (uint32_t)s->pc == 0x0e7742e0u)
      { uint32_t idx=(uint32_t)s->gpr[6], cnt=(uint32_t)s->gpr[23]; fprintf(stderr,"[BASEPROBE] base=%08x idx=%u count(s7)=%u %s icnt=%llu\n",(uint32_t)s->gpr[2],idx,cnt, idx>=cnt?"OOB!":"ok",(unsigned long long)s->icnt); }
  }
  { static const char* shp = getenv("SH_PROBE");
    static const uint32_t g_shp = shp ? (uint32_t)strtoul(shp,0,0) : 0;
    if(g_shp && (uint32_t)s->pc == g_shp)
      fprintf(stderr, "[SHPROBE] pc=%08x a0=%016llx a3=%016llx a4=%016llx sp=%016llx icnt=%llu\n",
        (uint32_t)s->pc, (unsigned long long)s->gpr[4], (unsigned long long)s->gpr[7],
        (unsigned long long)s->gpr[8], (unsigned long long)s->gpr[29], (unsigned long long)s->icnt); }

  /* SHLOOP: golden trace of the /sbin/sh list-walk (0x0e005974..0x0e005998) that the RTL
   * SIGSEGVs on -- e00597c `lw v1,0(v1)` faults with v1=0.  Logged BEFORE each loop pc, so
   * at e00597c v1(gpr[3]) is the pointer about to be deref'd; interp never faults so v1!=0
   * here.  Compare the a1(gpr[5]) sequence + v1 vs the RTL SHLOOP: same a1 + non-zero v1 =>
   * RTL load returned 0 (data bug); a1 diverges / extra iteration => a1/a0 corruption. */
  { static const bool g_shloop = getenv("SHLOOP") != nullptr;
    uint32_t pc = (uint32_t)s->pc;
    if(g_shloop && (pc==0x0e005974||pc==0x0e005978||pc==0x0e00597c||pc==0x0e00598c||pc==0x0e005990||pc==0x0e005998))
      fprintf(stderr, "[shloop] pc=%08x v0=%08x v1=%08x a0=%08x a1=%08x icnt=%llu\n",
        pc, (uint32_t)s->gpr[2], (uint32_t)s->gpr[3], (uint32_t)s->gpr[4], (uint32_t)s->gpr[5],
        (unsigned long long)s->icnt);
    // FIRST control-flow divergence loop @0x0e03ab50 (NULL-term array walk): s0=r16, v0=r2 (*s0), s1=r17.
    if(g_shloop && (pc==0x0e03ab50||pc==0x0e03ab64||pc==0x0e03ab68))
      fprintf(stderr, "[shdiv] pc=%08x s0=%08x v0=%08x s1=%08x a1=%08x icnt=%llu\n",
        pc, (uint32_t)s->gpr[16], (uint32_t)s->gpr[2], (uint32_t)s->gpr[17], (uint32_t)s->gpr[5],
        (unsigned long long)s->icnt); }

  /* SHTRACE: dump every /sbin/sh (0x0e000000..0x0e100000) userspace PC, to diff the golden
   * invocation's PC stream vs the RTL crashing invocation and find the FIRST divergence. */
  { static const bool g_shtrace = getenv("SHTRACE") != nullptr;
    uint32_t pc2 = (uint32_t)s->pc;
    if(g_shtrace && pc2>=0x0e000000u && pc2<0x0e100000u)
      fprintf(stderr, "[sht] %08x\n", pc2); }

  static const bool g_tally = getenv("TLBTALLY") != nullptr;
  if(g_tally) {
    if((uint32_t)s->pc == 0x880196bcu) { g_ty_tlbmiss++;
      uint32_t bv=(uint32_t)s->cpr0[CPR0_BADVADDR];
      if(bv>=0xffffa000u && bv<0xffffc000u){ g_ty_pda_miss++; if(g_ty_pda_miss<=8) fprintf(stderr,"[tally] PDA-region TLB miss bv=%08x icnt=%lu\n",bv,(unsigned long)s->icnt); }
      if(ty_idle_sp((uint32_t)s->gpr[29])) g_ty_idle_tlbmiss++; }
    if((s->icnt & 0x3ffffff) == 0) {
      fprintf(stderr, "[tally icnt=%lu] timer_irq=%lu tlbmiss=%lu idle_tlbmiss=%lu timer_in_tlbmiss=%lu timer_in_IDLE_tlbmiss=%lu pda_miss=%lu\n",
        (unsigned long)s->icnt,(unsigned long)g_ty_timer,(unsigned long)g_ty_tlbmiss,(unsigned long)g_ty_idle_tlbmiss,(unsigned long)g_ty_timer_in_tlbmiss,(unsigned long)g_ty_timer_idle,(unsigned long)g_ty_pda_miss);
      uint64_t pva=0xffffffffffffa020ULL; int pidx=-1; uint64_t phi=0,plo0=0,plo1=0;
      for(int i=0;i<state_t::NUM_TLB_ENTRIES;i++){
        uint64_t pm=s->tlb[i].page_mask&0x1ffe000ULL;
        uint64_t vm=(~(uint64_t)(pm|0x1fffULL))&0x000000ffffffe000ULL;
        uint64_t ehi=s->tlb[i].entry_hi;
        if(((pva&vm)==(ehi&vm))&&(((pva>>62)&3)==((ehi>>62)&3))){ pidx=i; phi=ehi; plo0=s->tlb[i].entry_lo0; plo1=s->tlb[i].entry_lo1; break; }
      }
      fprintf(stderr,"[tlbprobe] PDA(0xffffa020) idx=%d Wired=%u => %s  hi=%016llx lo0=%016llx lo1=%016llx\n",
        pidx,(unsigned)s->cpr0[CPR0_WIRED], (pidx>=0&&pidx<(int)s->cpr0[CPR0_WIRED])?"WIRED":(pidx>=0?"resident-NOT-wired":"ABSENT"),
        (unsigned long long)phi,(unsigned long long)plo0,(unsigned long long)plo1);
    }
  }

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
    if(g_tally) {
      if((cause & sr) & (1u<<15)) g_ty_timer++;
      if(ty_in_tlbmiss((uint32_t)s->pc)) { g_ty_timer_in_tlbmiss++;
        if(ty_idle_sp((uint32_t)s->gpr[29])) { g_ty_timer_idle++;
          fprintf(stderr, "[tally] *** TIMER-IN-IDLE-TLBMISS EPC=%08x sp=%08x icnt=%lu ***\n", (uint32_t)s->pc,(uint32_t)s->gpr[29],(unsigned long)s->icnt); } }
    }
    { static const bool g_ipprobe = getenv("IPPROBE") != nullptr;
      if(g_ipprobe) {
        uint32_t pc = (uint32_t)s->pc;
        if(pc >= 0x880034fcu && pc <= 0x88003600u) {   /* sthread_launch region */
          unsigned ip = (unsigned)(((cause & sr) >> 8) & 0xff);
          fprintf(stderr, "[ipprobe] INT @pc=%08x  IP=%02x (b7=timer b2=INT2/SCSI b1=SW1 b0=SW0)  icnt=%lu\n",
                  pc, ip, (unsigned long)s->icnt);
        }
      } }
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
/* non-faulting byte-wise guest memory access for the gdb stub. Translates each
 * byte's VA (handles page crossings) and touches raw guest bytes -- the guest
 * is big-endian, so the bytes are already in target order for gdb. */
bool gdb_mem_read(state_t *s, uint64_t va, uint8_t *buf, uint32_t len) {
  for(uint32_t i = 0; i < len; i++) {
    uint32_t pa;
    if(!tlb_probe_ro(s, va + i, &pa)) return false;
    buf[i] = *s->mem.get_raw_ptr(pa);
  }
  return true;
}
bool gdb_mem_write(state_t *s, uint64_t va, const uint8_t *buf, uint32_t len) {
  for(uint32_t i = 0; i < len; i++) {
    uint32_t pa;
    if(!tlb_probe_ro(s, va + i, &pa)) return false;
    *s->mem.get_raw_ptr(pa) = buf[i];
  }
  return true;
}
/* One-shot dump of the IRIX "current process" -- comm name + pid + the pc/mode
 * it is executing. Wired to SIGUSR1 in main() so the live emulator can be asked
 * "what is IRIX running right now?" from another shell (kill -USR1 <pid>). */
void dump_current_process(state_t *s) {
#ifdef ENABLE_O32_TRACE
  /* interrupted PC (EPC) + RA + a scan of the kernel stack for return addresses.
   * When SIGUSR1 lands inside an interrupt handler, EPC is the PC that was
   * preempted (the wait loop); the stack scan gives a rough backtrace. Works even
   * in early init where there is no current uthread. */
  {
    uint32_t epc = (uint32_t)s->cpr0[CPR0_EPC];
    uint32_t ra  = (uint32_t)s->gpr[31];
    uint32_t sp  = (uint32_t)s->gpr[29];
    uint32_t s0 = (uint32_t)s->gpr[16];    /* lcl_intrd's interrupt descriptor ptr */
    uint32_t hdlr = 0, lvl = 0, mbit = 0;
    guest_rd32_be(s, (uint64_t)(int64_t)(int32_t)(s0 +  0), &hdlr);  /* handler fn */
    guest_rd32_be(s, (uint64_t)(int64_t)(int32_t)(s0 +  8), &mbit);  /* mask bit */
    guest_rd32_be(s, (uint64_t)(int64_t)(int32_t)(s0 + 12), &lvl);   /* local level 0/1/2 */
    fprintf(stderr, "[spin] icnt=%lu pc=%08x epc=%08x ra=%08x s0=%08x hdlr=%08x lvl=%u mbit=%08x sp=%08x stack:",
            (unsigned long)s->icnt, (uint32_t)s->pc, epc, ra, s0, hdlr, lvl, mbit, sp);
    for(int i = 0; i < 96; i++) {
      uint32_t w;
      if(!guest_rd32_be(s, (uint64_t)(int64_t)(int32_t)(sp + i * 4), &w)) break;
      if(w >= 0x88002180u && w < 0x882aa180u) fprintf(stderr, " %08x", w);
    }
    fprintf(stderr, "\n");
  }
#endif
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
/* TRACEWIN_SEGV: freeze the retire-trace ring the instant an o32 (user) process
 * faults to a wild address (near-null or a kernel address = the classic bad-pointer
 * SEGV), so the ring holds the o32 run-up and not the post-crash delayloop. */
static bool g_ring_frozen = false;

static void raise_tlb(state_t *s, uint64_t va, uint32_t exccode,
                      bool is_refill, bool is_xtlb) {
  static const bool tlbdbg = getenv("TLBDBG") != nullptr;
#ifdef ENABLE_O32_TRACE
  { static const bool seg_trig = getenv("TRACEWIN_SEGV") != nullptr;
    static uint64_t seg_floor = getenv("TRACEWIN_SEGV_FLOOR") ? strtoull(getenv("TRACEWIN_SEGV_FLOOR"),0,0) : 0;
    /* A user process faulting to a KERNEL address (>=0x80000000, always an AdE bug)
     * or the null page (<0x1000) = the classic bad-pointer SEGV. Floor skips early boot.
     * Gate on USER mode (KSU==2 & !EXL & !ERL) -- raise_tlb runs before EXL is set, so
     * Status still reflects the faulting context. This catches the o32 process's OWN
     * fault (pmie) and excludes the downstream kernel derail (pc=0/ra=0, kernel mode). */
    uint32_t seg_sr = s->cpr0[CPR0_SR];
    bool seg_umode = (((seg_sr >> 3) & 3u) == 2u) && !(seg_sr & SR_EXL) && !(seg_sr & SR_ERL);
    if(seg_trig && !g_ring_frozen && seg_umode && (uint32_t)s->pc < 0x80000000u
       && s->icnt >= seg_floor && ((uint32_t)va < 0x00001000u || (uint32_t)va >= 0x80000000u)) {
      g_ring_frozen = true;
      uint32_t inst = 0; guest_rd32_be(s, s->pc, &inst);
      uint32_t base = (inst >> 21) & 0x1f, rt = (inst >> 16) & 0x1f;
      fprintf(stderr, "[segv-trig] FROZE ring: user pc=%08x badaddr=%08x code=%u icnt=%lu inst=%08x op=%u base=r%u(=%08x) rt=r%u\n",
              (uint32_t)s->pc, (uint32_t)va, exccode, (unsigned long)s->icnt, inst,
              inst >> 26, base, (uint32_t)s->gpr[base], rt);
      fprintf(stderr, "[segv-trig] gpr:");
      for(int i = 0; i < 32; i++) fprintf(stderr, " r%d=%08x", i, (uint32_t)s->gpr[i]);
      fprintf(stderr, "\n");
      /* t9-null-call case (pc==0 = jalr t9 with t9==0): dump the caller's code around
       * ra, and find the `lw t9, off(gp)` (PIC GOT load) -> read the GOT word it loaded.
       * GOTWORD==0 => rld didn't relocate (dynamic-linking bug); GOTWORD!=0 but t9==0
       * => interp mis-loaded the GOT word (32b load bug). */
      if((uint32_t)s->pc == 0) {
        uint32_t ra = (uint32_t)s->gpr[31], gp = (uint32_t)s->gpr[28];
        fprintf(stderr, "[segv-trig] caller code (ra-64..ra):\n");
        for(int k = 16; k >= 0; k--) {
          uint32_t ci = 0; guest_rd32_be(s, ra - k*4, &ci);
          fprintf(stderr, "   %08x: %08x%s\n", ra - k*4, ci,
                  ((ci >> 26) == 0x23 && ((ci >> 16) & 0x1f) == 25) ? "   <- lw t9" :
                  ((ci & 0xfc1fffff) == 0x0320f809) ? "   <- jalr t9" : "");
        }
        for(int k = 2; k <= 64; k++) {   /* search back for lw t9, off(gp) */
          uint32_t ci = 0; guest_rd32_be(s, ra - k*4, &ci);
          if((ci >> 26) == 0x23 && ((ci >> 16) & 0x1f) == 25 && ((ci >> 21) & 0x1f) == 28) {
            int32_t off = (int16_t)(ci & 0xffff);
            uint32_t gotaddr = gp + off, gotword = 0; guest_rd32_be(s, gotaddr, &gotword);
            fprintf(stderr, "[GOT] lw t9 @%08x off=%d gp=%08x -> gotaddr=%08x GOTWORD=%08x\n",
                    ra - k*4, off, gp, gotaddr, gotword);
            break;
          }
        }
      }
      else {
        /* data-null-deref case: dump code around the faulting pc so the sequence
         * that computed the null base register (r%base) is visible. */
        fprintf(stderr, "[segv-trig] faulting code (pc-32..pc+4), null base=r%u:\n", base);
        for(int k = 8; k >= -1; k--) {
          uint32_t ci = 0; guest_rd32_be(s, (uint32_t)s->pc - k*4, &ci);
          fprintf(stderr, "   %08x: %08x%s\n", (uint32_t)s->pc - k*4, ci,
                  (k == 0) ? "   <- FAULT" : "");
        }
      }
      /* WRTRACK unified: follow the nullptr backwards regardless of manifestation.
       * Identify the null pointer register -- data-deref: the faulting load's base;
       * pc==0: `jr ra` (r31==0) or `jalr t9` (r25==0) -- then use per-GPR load
       * provenance to find the memory word it was loaded from, and the LAST STORE
       * to that word. NEVER WRITTEN (pc/icnt 0) => the null is the zero-fill: a
       * MISSING init/relocation. A surprising writer PC => that store is the
       * corruptor. */
      if(g_wrtrack) {
        int nr = -1;
        if((uint32_t)s->pc == 0) {
          nr = (s->gpr[31] == 0) ? 31 : (s->gpr[25] == 0 ? 25 : -1);
        }
        else {
          nr = (int)base;
        }
        if(nr >= 0 && g_gpr_ld_pc[nr] != 0) {
          uint32_t pa = g_gpr_ld_pa[nr]; uint64_t w = (uint64_t)pa >> 2;
          fprintf(stderr, "[WRTRACK] null r%d last LOADED from VA %08x (PA %08x) by pc %08x\n",
                  nr, g_gpr_ld_va[nr], pa, g_gpr_ld_pc[nr]);
          fprintf(stderr, "[WRTRACK]   last STORE to that word: pc=%08x icnt=%lu%s\n",
                  g_wr_pc[w], (unsigned long)g_wr_icnt[w],
                  (g_wr_pc[w] == 0 && g_wr_icnt[w] == 0) ? "  (NEVER WRITTEN -> zero-fill; missing init/reloc)" : "");
        }
        else if(nr >= 0) {
          fprintf(stderr, "[WRTRACK] null r%d has no load-provenance (set by ALU/jal, not a load)\n", nr);
        }
      }
    } }
#endif
  bool exl_was_set = (s->cpr0[CPR0_SR] & SR_EXL) != 0;
  tlb_set_fault_state(s, va);
  { /* FAULTLOG: per-fault (cause,badaddr,pc) so re-fault counts can be compared vs the RTL. */
    static const bool fl = getenv("FAULTLOG") != nullptr;
    if(fl) fprintf(stderr, "[fault] cause=%u badaddr=%08x pc=%08x refill=%d icnt=%lu\n",
                   exccode, (uint32_t)va, (uint32_t)s->pc, is_refill?1:0, (unsigned long)s->icnt);
#ifdef ENABLE_O32_TRACE
    /* FAULTLOG_USER: user-mode faults only (find the o32 SIGSEGV without the kernel refill flood).
     * FAULTLOG_FLOOR skips early boot; true user-mode gate (KSU==2 & !EXL & !ERL) drops the
     * kernel-refill / kernel-derail noise so only the o32 process's own faults show. */
    static const bool flu = getenv("FAULTLOG_USER") != nullptr;
    static const uint64_t flu_floor = getenv("FAULTLOG_FLOOR") ? strtoull(getenv("FAULTLOG_FLOOR"),0,0) : 0;
    uint32_t flu_sr = s->cpr0[CPR0_SR];
    bool flu_umode = (((flu_sr >> 3) & 3u) == 2u) && !(flu_sr & SR_EXL) && !(flu_sr & SR_ERL);
    if(flu && flu_umode && s->icnt >= flu_floor)
      fprintf(stderr, "[ufault] pc=%08x badaddr=%08x code=%u refill=%d EPC=%08x icnt=%lu\n",
              (uint32_t)s->pc, (uint32_t)va, exccode, is_refill?1:0, s->cpr0[CPR0_EPC], (unsigned long)s->icnt);
#endif
    }
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
struct utlb_entry { uint64_t vpn, asid; uint32_t ppn; bool dirty, valid, cached; };
static utlb_entry g_utlb[UTLB_SZ];
static inline void utlb_flush() { for(auto &e : g_utlb) e.valid = false; }

/* TLBDUP (env-gated): after a TLBWI/TLBWR installs entry `idx`, scan for ANOTHER
 * slot that ALSO matches on VPN2[39:13]+R[63:62]+(ASID or both-Global), with both
 * pages considered valid iff V0|V1 -- i.e. the r9999 CAM would light up two match
 * lines.  IRIX is duplicate-safe by design (immu.h: probe-first tlbdropin + blind
 * tlbwr only on a genuine refill), so if the golden ISS NEVER fires this while the
 * RTL does, the duplicate is an r9999 artifact (a spurious refill on a resident
 * VA), confirming find_lowest_set_bit neutralizes rather than merely papers over.
 * Mirrors henry_tb.cpp's TLBDUP so the two are apples-to-apples. */
static void iss_tlbdup_check(state_t *s, uint32_t idx, const char *how) {
  static const bool en = getenv("TLBDUP") != nullptr;
  if(!en) return;
  uint64_t ehi = s->tlb[idx].entry_hi, lo0 = s->tlb[idx].entry_lo0, lo1 = s->tlb[idx].entry_lo1;
  uint64_t vpn2 = (ehi >> 13) & 0x7ffffffULL, r = (ehi >> 62) & 0x3ULL, asid = ehi & 0xffULL;
  bool gl = (lo0 & 1u) && (lo1 & 1u);
  bool vld = ((lo0 >> 1) & 1u) || ((lo1 >> 1) & 1u);
  if(!vld) return;                                   /* skip invalid placeholder writes */
  for(int j = 0; j < state_t::NUM_TLB_ENTRIES; j++) {
    if((uint32_t)j == idx) continue;
    uint64_t jhi = s->tlb[j].entry_hi, jl0 = s->tlb[j].entry_lo0, jl1 = s->tlb[j].entry_lo1;
    bool jvld = ((jl0 >> 1) & 1u) || ((jl1 >> 1) & 1u);
    if(!jvld) continue;
    bool jgl = (jl0 & 1u) && (jl1 & 1u);
    bool va_match = (((jhi >> 13) & 0x7ffffffULL) == vpn2) && (((jhi >> 62) & 0x3ULL) == r);
    bool ai_match = ((jhi & 0xffULL) == asid) || (gl && jgl);
    if(va_match && ai_match)
      fprintf(stderr, "[ISS-TLBDUP] %s vpn2=%llx r=%llu asid=%02llx : entry %u (lo0=%llx lo1=%llx) == entry %d (lo0=%llx lo1=%llx) icnt=%llu\n",
        how, (unsigned long long)vpn2, (unsigned long long)r, (unsigned long long)asid,
        idx, (unsigned long long)lo0, (unsigned long long)lo1,
        j, (unsigned long long)jl0, (unsigned long long)jl1, (unsigned long long)s->icnt);
  }
}

/* ---- L1 VIPT alias-frequency instrument (env L1_ALIAS) --------------------
 * Measures how often the SAME physical line is resident under two DIFFERENT VA
 * alias-indices -- i.e. how often IRIX's page coloring fails to keep a VIPT L1
 * alias-free.  That count IS the back-invalidate rate an inclusive-L2 alias-
 * resolution scheme would incur at a given L1 size.  One run sweeps several L1
 * sizes (16B lines, direct-mapped = r9999 L1D geometry; a 4KB L1's index is a
 * subset of the 4KB page offset so it can never alias -> sanity 0).  Fed every
 * cacheable DATA access (va,pa) from va_translate. */
namespace {
  struct alias_l1 {
    static const uint32_t NOVAL = 0xffffffffu;
    uint32_t kb = 0, lg_sets = 0;
    std::vector<uint32_t> tag;               /* resident line_pa per set (NOVAL = empty) */
    uint64_t n_back_inval = 0;
    void init(uint32_t kb_) {
      kb = kb_;
      uint32_t sets = (kb * 1024u) / 16u;    /* 16B lines */
      lg_sets = 0; while((1u << lg_sets) < sets) { lg_sets++; }
      tag.assign(1u << lg_sets, NOVAL);
    }
    void access(uint32_t va, uint32_t pa) {
      uint32_t set = (va >> 4) & ((1u << lg_sets) - 1);
      uint32_t lpa = pa >> 4;
      if(tag[set] == lpa) { return; }        /* hit at this alias-index */
      if(lg_sets > 8) {                      /* >4KB: alias index bits (>= bit 8) exist */
        uint32_t inpage = set & 0xffu;       /* page-offset index bits [11:4] = 256 lines/4KB page */
        for(uint32_t a = 0; a < (1u << (lg_sets - 8)); a++) {
          uint32_t aset = inpage | (a << 8);
          if(aset != set && tag[aset] == lpa) { n_back_inval++; tag[aset] = NOVAL; }
        }
      }
      tag[set] = lpa;                        /* fill (conflict-evict implicit) */
    }
  };
  bool g_alias_init = false, g_alias_on = false;
  std::vector<alias_l1> g_alias_d, g_alias_i;   /* D-side and I-side sweeps */
  uint64_t g_alias_d_n = 0, g_alias_i_n = 0;
  inline void alias_ensure_init() {
    if(g_alias_init) { return; }
    g_alias_init = true;
    if(getenv("L1_ALIAS")) {
      g_alias_on = true;
      for(uint32_t kb : {4u, 8u, 16u, 32u, 64u}) {
        alias_l1 md; md.init(kb); g_alias_d.push_back(md);
        alias_l1 mi; mi.init(kb); g_alias_i.push_back(mi);
      }
    }
  }
  inline void alias_access(uint32_t va, uint32_t pa) {         /* D-side (load/store) */
    alias_ensure_init(); if(!g_alias_on) { return; }
    g_alias_d_n++; for(auto &m : g_alias_d) { m.access(va, pa); }
  }
  inline void alias_fetch_access(uint32_t va, uint32_t pa) {   /* I-side (fetch) */
    alias_ensure_init(); if(!g_alias_on) { return; }
    g_alias_i_n++; for(auto &m : g_alias_i) { m.access(va, pa); }
  }
}
void l1_alias_report() {
  if(!g_alias_on) { return; }
  auto dump = [](const char *side, std::vector<alias_l1> &v, uint64_t n) {
    fprintf(stderr, "\n[L1_ALIAS] %s-side cacheable accesses = %llu\n", side, (unsigned long long)n);
    for(auto &m : v) {
      fprintf(stderr, "[L1_ALIAS]  %s %2u KB (%u sets, %u alias bits): back-invalidates = %llu  (%.3e/access; 1 per %.0f)\n",
              side, m.kb, 1u << m.lg_sets, (m.lg_sets > 8 ? m.lg_sets - 8 : 0),
              (unsigned long long)m.n_back_inval,
              n ? (double)m.n_back_inval / (double)n : 0.0,
              m.n_back_inval ? (double)n / (double)m.n_back_inval : 0.0);
    }
  };
  dump("D", g_alias_d, g_alias_d_n);
  dump("I", g_alias_i, g_alias_i_n);
}

static uint32_t va_translate_inner(state_t *s, uint64_t va, tlb_op op) {
  uint32_t hi32 = (uint32_t)(va >> 32);
  uint32_t lo32 = (uint32_t)va;

  /* WATCHC4: log every store/load to the /sbin/sh crash slot 0x0e0981c4 (node-1b8 +12 field
   * that the RTL misreads as 0 vs golden 0x0e07bbc0) with pc+icnt, to find the build store's
   * PC and the store->crash-load instruction distance. */
  { static const bool g_watchc4 = getenv("WATCHC4") != nullptr;
    // STORES to the whole node-1b8 region [0e0981b8,0e0981c8) (catches sd@c0, sw@c4, unaligned,
    // or a block copy), plus the exact-c4 loads for the store->load distance.
    if(g_watchc4 && op == tlb_op::store && lo32 >= 0x0e0981b8u && lo32 < 0x0e0981c8u)
      fprintf(stderr, "[watchc4] STORE pc=%08x va=%08x icnt=%llu\n",
        (uint32_t)s->pc, lo32, (unsigned long long)s->icnt);
    if(g_watchc4 && op == tlb_op::load && lo32 == 0x0e0981c4u)
      fprintf(stderr, "[watchc4] load  pc=%08x va=%08x icnt=%llu\n",
        (uint32_t)s->pc, lo32, (unsigned long long)s->icnt); }

  /* IDXWR: shadow last-writer-PC per word (keyed on VA), to find what STORES the
   * array-insert index mem[s4] loaded at 0e774150.  On each store record the storing
   * PC for the base word (+next word to cover sd); at the idx-load print the writer. */
  { static const bool g_idxwr = getenv("IDXWR") != nullptr;
    static std::unordered_map<uint32_t,uint32_t> lastwr;
    if(g_idxwr) {
      if(op == tlb_op::store) {
        lastwr[lo32 & ~3u] = (uint32_t)s->pc;   /* exact word only (no +4 over-record) */
      }
      if(op == tlb_op::load && (uint32_t)s->pc == 0x0e774150u) {
        auto it = lastwr.find(lo32 & ~3u);
        fprintf(stderr, "[idxwr] idx-load va=%08x last_writer_pc=%08x icnt=%llu\n",
          lo32, (it==lastwr.end()?0xffffffffu:it->second), (unsigned long long)s->icnt);
      }
      /* CAPWR: at the capacity load (lw t0,4(v0) @0e774154) lo32 = table+4 = the
       * capacity field; report who last WROTE it -> golden's capacity-sizing site. */
      if(op == tlb_op::load && (uint32_t)s->pc == 0x0e774154u) {
        auto it = lastwr.find(lo32 & ~3u);
        fprintf(stderr, "[capwr] cap-load va=%08x last_writer_pc=%08x icnt=%llu\n",
          lo32, (it==lastwr.end()?0xffffffffu:it->second), (unsigned long long)s->icnt);
      }
    }
  }

  /* cache_model: default this access to uncached; the cached returns below set it.
   * Never cache an instruction fetch (D-cache-only model for now). */
  s->mem.cache_active = false;

  /* 32-bit compatibility segments (sign-extended VA: hi32 is 0x00000000 for
   * useg or 0xffffffff for kseg0..kseg3). */
  if(hi32 == 0x00000000u || hi32 == 0xffffffffu) {
    uint32_t seg = lo32 >> 29;
    /* kseg0 (0x80000000-0x9fffffff) and kseg1 (0xa0000000-0xbfffffff): unmapped */
    if(seg == 0x4 || seg == 0x5) {
      if(seg == 0x4 && op != tlb_op::fetch) { s->mem.cache_active = true; } /* kseg0 cached */
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
    if(ce.cached && op != tlb_op::fetch) { s->mem.cache_active = true; }
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

    /* HEAP_PROBE: page size + even/odd + D-bit for /sbin/sh heap stores.  RTL hardcodes
     * w_odd=va[12] (4KB); if pm!=0 here the RTL selects the wrong half's D -> spurious cause-1. */
    { static const bool g_hp = getenv("HEAP_PROBE") != nullptr;
      if(g_hp && op == tlb_op::store && va >= 0x0e0a0000ULL && va < 0x0e0b0000ULL)
        fprintf(stderr, "[HEAPPROBE] va=%09llx pm=%llx sel_bit=%llx odd=%d D=%d V=%d e_lo0=%llx e_lo1=%llx icnt=%llu\n",
          (unsigned long long)va, (unsigned long long)pm, (unsigned long long)sel_bit, (int)odd,
          (int)((e_lo>>2)&1), (int)((e_lo>>1)&1),
          (unsigned long long)s->tlb[i].entry_lo0, (unsigned long long)s->tlb[i].entry_lo1,
          (unsigned long long)s->icnt); }

    if(!(e_lo & 0x2u)) {                            /* V == 0 -> TLB Invalid */
      uint32_t code = (op == tlb_op::store) ? 3u : 2u;
      raise_tlb(s, va, code, /*is_refill=*/false, xtlb);
      return 0;
    }
    if(op == tlb_op::store && !(e_lo & 0x4u)) {     /* D == 0 -> TLB Modified */
      { static const bool g_ht = getenv("HTRACE") != nullptr;   /* trace the kernel TLB-Mod handler for /sbin/sh heap */
        if(g_ht && va >= 0x0e0a0000ULL && va < 0x0e0b0000ULL) g_htrace = 320; }
#ifdef ENABLE_O32_TRACE
      uint32_t modpc = (uint32_t)s->pc;             /* faulting pc, before raise_tlb rewrites s->pc */
      uint64_t modva = va;
#endif
      raise_tlb(s, va, 1u, /*is_refill=*/false, xtlb);
#ifdef ENABLE_O32_TRACE
      /* MODLOG: dump user (o32) TLB-Modified exceptions -- the full 64b store VA and
       * the BadVAddr/Context/EntryHi the kernel tlbmod handler walks. A 32-bit
       * sign-extend/truncate bug shows up as a bad VA or a bad Context here. */
      { static const bool g_ml = getenv("MODLOG") != nullptr;
        static uint64_t g_mlf = getenv("MODLOG_FLOOR") ? strtoull(getenv("MODLOG_FLOOR"),0,0) : 0;
        static int g_nml = 0;
        if(g_ml && modpc < 0x80000000u && s->icnt >= g_mlf && g_nml++ < 80)
          fprintf(stderr, "[MOD] pc=%08x va=%016llx e_lo=%016llx badv=%016llx ctx=%016llx ehi=%016llx icnt=%lu\n",
                  modpc, (unsigned long long)modva, (unsigned long long)e_lo,
                  (unsigned long long)s->cpr0_64[CPR0_BADVADDR], (unsigned long long)s->cpr0_64[CPR0_CONTEXT],
                  (unsigned long long)s->cpr0_64[CPR0_ENTRYHI], (unsigned long)s->icnt); }
#endif
      return 0;
    }
    uint64_t pfn = (e_lo >> 6) & 0xfffffffULL;      /* PFN[33:6] -> 28 bits */
    uint64_t pa  = (pfn << 12) | (va & off_mask);
#ifdef ENABLE_O32_TRACE
    /* DEVMAP: a MAPPED user access must NOT translate into the device-MMIO window
     * 0x1f000000-0x1fffffff (that's where the o32 pointers vanish). Dump the matched
     * TLB entry so we can tell a corrupt-PFN entry (bug at TLB-write time) from a
     * mis-selected/mis-extracted PA (bug here). */
    { static const bool g_dm = getenv("DEVMAP") != nullptr;
      if(g_dm && (uint32_t)pa >= 0x1f000000u && (uint32_t)pa <= 0x1fffffffu && (uint32_t)s->pc < 0x80000000u) {
        static int g_ndm = 0;
        if(g_ndm++ < 40)
          fprintf(stderr, "[DEVMAP] pc=%08x va=%016llx -> PA %08x  tlb[%d] e_hi=%016llx e_lo0=%016llx e_lo1=%016llx pm=%08x odd=%d pfn=%llx icnt=%lu\n",
                  (uint32_t)s->pc, (unsigned long long)va, (uint32_t)pa, i,
                  (unsigned long long)s->tlb[i].entry_hi,
                  (unsigned long long)s->tlb[i].entry_lo0, (unsigned long long)s->tlb[i].entry_lo1,
                  s->tlb[i].page_mask, (int)odd, (unsigned long long)pfn, (unsigned long)s->icnt);
      }
    }
#endif
    bool cca_cached = (((e_lo >> 3) & 7u) == 3u);    /* C==3: cacheable write-back */
    /* fill the micro-TLB for this 4KB sub-page (only reached for a V=1 hit) */
    ce.vpn = vpn; ce.asid = cur_asid; ce.ppn = (uint32_t)(pa >> 12);
    ce.dirty = (e_lo & 0x4u) != 0; ce.valid = true; ce.cached = cca_cached;
    if(cca_cached && op != tlb_op::fetch) { s->mem.cache_active = true; }
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

/* PTETRACK (WRTRACK): a MTC0/DMTC0 that writes EntryLo0/1 with a device-space PFN
 * (value bit 22 = PFN bit 16 = PA bit 28, V=1) is installing a user page into the
 * 0x1f000000 MMIO window. Follow it back: the source GPR's load provenance gives the
 * PTE address; read the PTE still in memory and compare. MEM already has bit 28 =>
 * the kernel BUILT the PTE wrong (chase the writer pc). MEM clean => interp corrupted
 * it on the refill LOAD (bug at g_gpr_ld_pc). */
#ifdef ENABLE_O32_TRACE
static void ptetrack_probe(state_t *s, uint32_t rd, uint32_t rt) {
  if(!g_wrtrack) { return; }
  if(!(rd == CPR0_ENTRYLO0 || rd == CPR0_ENTRYLO1)) { return; }
  uint32_t v = (uint32_t)s->gpr[rt];
  uint32_t vpfn = (v >> 6) & 0xfffffffu;
  if(!(vpfn >= 0x1f000u && vpfn <= 0x1ffffu && ((v >> 1) & 1u))) { return; }  /* PA in device window 0x1f000000-0x1fffffff, V=1 */
  static int n = 0;
  if(n++ >= 40) { return; }
  uint32_t ldva = g_gpr_ld_va[rt], ldpa = g_gpr_ld_pa[rt], ldpc = g_gpr_ld_pc[rt];
  uint32_t ptemem = 0;
  if(ldva) { guest_rd32_be(s, (uint64_t)(int64_t)(int32_t)ldva, &ptemem); }
  uint64_t w = (uint64_t)ldpa >> 2;
  fprintf(stderr, "[PTETRACK] EntryLo%d=%08x (dev PFN) via r%u pc=%08x icnt=%lu\n",
          (rd == CPR0_ENTRYLO0) ? 0 : 1, v, rt, (uint32_t)s->pc, (unsigned long)s->icnt);
  fprintf(stderr, "[PTETRACK]   r%u last LOADED from VA %08x (PA %08x) by pc %08x; PTE-in-mem=%08x  %s\n",
          rt, ldva, ldpa, ldpc, ptemem,
          !ldpc ? "(no load provenance -- value came via ALU, not a PTE load)" :
          (((ptemem >> 22) & 1u) ? "(MEM HAS bit28 -> kernel built PTE wrong)"
                                 : "(MEM CLEAN -> interp corrupted it on the load)"));
  fprintf(stderr, "[PTETRACK]   last STORE to that PTE word: pc=%08x icnt=%lu%s\n",
          g_wr_pc[w], (unsigned long)g_wr_icnt[w],
          (g_wr_pc[w] == 0 && g_wr_icnt[w] == 0) ? "  (never written -> zero-fill)" : "");
}
#else
static inline void ptetrack_probe(state_t *, uint32_t, uint32_t) {}
#endif

/* wrapper: feed every successful cacheable DATA access (va,pa) to the VIPT
 * alias-frequency instrument, then return the PA unchanged. */
static uint32_t va_translate(state_t *s, uint64_t va, tlb_op op) {
  uint32_t pa = va_translate_inner(s, va, op);
  if(!s->tlb_fault) {
    if(op != tlb_op::fetch) {
      if(s->mem.cache_active) { alias_access((uint32_t)va, pa); }   /* D-side */
    } else {
      /* I-side: cacheable fetch = not the kseg1 uncached window (PROM) */
      uint32_t lo = (uint32_t)va, hi = (uint32_t)(va >> 32);
      bool kseg1 = (hi == 0 || hi == 0xffffffffu) && ((lo >> 29) == 5u);
      if(!kseg1) { alias_fetch_access(lo, pa); }
    }
  }
  return pa;
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
    /* BLTZALL/BGEZALL: the link (ra <- pc+8) is UNCONDITIONAL -- only the branch
     * direction depends on the condition.  Must fire even when NOT taken. */
    if(saveReturn)
      s->gpr[31] = npc + 4;   /* full 64-bit link (n64 user PCs exceed 32 bits) */
    if(takeBranch) {
      if(!run_delay_slot<EL>(s))
	s->pc = (imm+npc);
    }
    else {
      s->pc += 4;
    }
  }
  else {
    bool ds_faulted = run_delay_slot<EL>(s);
    /* BLTZAL/BGEZAL: link UNCONDITIONALLY (not gated on takeBranch).  The PIC
     * bootstrap idiom `bltzal rX,.+8` with rX>=0 is the not-taken-but-must-link
     * case -- gating the link on takeBranch left ra=0 -> wrong load bias. */
    if(saveReturn) {
      s->gpr[31] = npc + 4;   /* full 64-bit link (n64 user PCs exceed 32 bits) */
    }
    if(takeBranch){
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
#ifdef ENABLE_O32_TRACE
  if(unlikely(g_wrtrack)) { g_gpr_ld_va[rt] = (uint32_t)(s->gpr[rs] + imm); g_gpr_ld_pa[rt] = ea; g_gpr_ld_pc[rt] = (uint32_t)s->pc; }
#endif
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
#ifdef ENABLE_O32_TRACE
  if(unlikely(g_wrtrack)) { g_gpr_ld_va[rt] = (uint32_t)(s->gpr[rs] + imm); g_gpr_ld_pa[rt] = ea; g_gpr_ld_pc[rt] = (uint32_t)s->pc; }
#endif
  s->pc += 4;
  HISTO(s, mipsInsn::LD);
}

template <bool EL>
void _sc(uint32_t inst, state_t *s) {
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  { static const bool g_t = getenv("LLSC_TRACE")!=nullptr; static uint64_t nk=0,nu=0;
    bool user = ((uint32_t)s->pc) < 0x80000000u;
    if(user) nu++; else nk++;
    if(g_t && (user || ((nk&0xffff)==0)))
      fprintf(stderr,"[SC] %s pc=%08x ea=%08x val=%08x icnt=%llu (user=%llu kern=%llu)\n",
        user?"USER":"kern",(uint32_t)s->pc,(uint32_t)(s->gpr[rs]+imm),(uint32_t)s->gpr[rt],
        (unsigned long long)s->icnt,(unsigned long long)nu,(unsigned long long)nk); }
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
  { static const bool g_dis = getenv("DISPROBE") != nullptr;
    static uint32_t seen[128]; static int nseen=0;
    uint32_t pc32=(uint32_t)s->pc;
    bool inrange = (pc32>=0x0e636948u && pc32<0x0e6369c0u) || (pc32>=0x0e635d00u && pc32<0x0e635d60u);
    if(g_dis && inrange) {
      bool dup=false; for(int k=0;k<nseen;k++) if(seen[k]==pc32){dup=true;break;}
      if(!dup && nseen<128){ seen[nseen++]=pc32;
        fprintf(stderr,"[dis] %08x  %08x\n", pc32, inst); }
    }
  }
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
  { /* PCHASH: rolling FNV-1a over the retired-PC stream, checkpoint every 65536.
     * Compared against henry_tb's identical hash to find the first RTL/ISS PC
     * divergence (expected only once interrupts skew the streams). */
    static const bool g_pchash = getenv("PCHASH") != nullptr;
    if(g_pchash) {
      static uint64_t h = 1469598103934665603ULL;
      h = (h ^ (uint32_t)s->pc) * 1099511628211ULL;
      if((s->icnt & 0xffff) == 0) {
        fprintf(stderr, "[pchash] icnt=%llu hash=%016llx pc=%08x\n",
                (unsigned long long)s->icnt, (unsigned long long)h, (uint32_t)s->pc);
      }
    }
    static const char *pd = getenv("PCDUMP");   /* "lo:hi" -> raw "P <pc>" for structural diff */
    static uint64_t pd_lo = 0, pd_hi = 0; static bool pd_init = false;
    if(!pd_init) { pd_init = true; if(pd) sscanf(pd, "%lu:%lu", &pd_lo, &pd_hi); }
    if(pd && s->icnt >= pd_lo && s->icnt < pd_hi) {
      fprintf(stderr, "P %08x\n", (uint32_t)s->pc);
    }
  }
  if(globals::trace_retirement and globals::retire_log) {
    /* optional TRACEWIN=lo:hi icnt window so a huge boot can emit a small,
     * focused retire_trace (e.g. around a single XTLB refill) for mips-analyzer. */
    static bool tw_init = false; static uint64_t tw_lo = 0, tw_hi = ~0ULL, tw_cap = 0, tw_ring = 0;
    static bool tw_useronly = false, tw_trig_user = false, tw_armed = false;
    if(!tw_init) { tw_init = true; const char *tw = getenv("TRACEWIN");
                   if(tw) sscanf(tw, "%lu:%lu", &tw_lo, &tw_hi);
                   tw_useronly = getenv("TRACEWIN_USERONLY") != nullptr;
                   tw_trig_user = getenv("TRACEWIN_TRIG_USER") != nullptr;      // arm on first o32 entry
                   if(getenv("TRACEWIN_N")) tw_cap = strtoull(getenv("TRACEWIN_N"),0,0);
                   if(getenv("TRACEWIN_RING")) tw_ring = strtoull(getenv("TRACEWIN_RING"),0,0);  // keep LAST N
                   if(tw || tw_ring) fprintf(stderr, "[tracewin] lo=%llu hi=%llu useronly=%d cap=%llu ring=%llu\n",
                                  (unsigned long long)tw_lo, (unsigned long long)tw_hi, tw_useronly,
                                  (unsigned long long)tw_cap, (unsigned long long)tw_ring); }
    if(tw_trig_user && !tw_armed && (uint32_t)s->pc < 0x80000000u) tw_armed = true;
    if(!g_ring_frozen && s->icnt >= tw_lo && s->icnt < tw_hi && (!tw_trig_user || tw_armed)
       && (tw_ring != 0 || tw_cap == 0 || globals::retire_log->get_records().size() < tw_cap)
       && !(tw_useronly && (uint32_t)s->pc >= 0x80000000u)) {
      auto &recs = globals::retire_log->get_records();
      recs.emplace_back(ipa, (uint64_t)s->pc, inst);
      /* TRACEWIN_RING=N: keep only the last N records (trace the run-up to a crash,
       * regardless of exact icnt). std::list pop_front is O(1). */
      if(tw_ring != 0 && recs.size() > tw_ring) recs.pop_front();
    }
  }
  if(globals::pctrace) {
    static const bool useronly = getenv("PCTRACE_USERONLY") != nullptr;  /* only pc<0x80000000 */
    if(!globals::pctrace_on && (uint32_t)s->pc == globals::pctrace_start) globals::pctrace_on = true;
    if(globals::pctrace_on && !(useronly && (uint32_t)s->pc >= 0x80000000u))
      fprintf(globals::pctrace, "%08x\n", (uint32_t)s->pc);
  }
  /* TEMP: at resumeidle+0x30 (0x88003568, just after `lw sp,-24156(zero)`), dump the
   * idle sp the ISS loads + the source global mem[0xFFFFA164]. FPGA got 0x8834afb8. */
  if((uint32_t)s->pc==0x88002200u){
    static int hc=0;
    if(hc++ < 8){
      uint32_t sp=(uint32_t)s->gpr[29];
      uint64_t va=(uint64_t)(int64_t)(-24156); uint32_t pa=va_translate(s, va, tlb_op::load);
      uint32_t g=__builtin_bswap32(s->mem.get<uint32_t>(pa));
      fprintf(stderr,"[GLOB%d] icnt=%llu mem[0xFFFFA164] pa=%08x val=%08x (idle sp global; FPGA loaded 0x8834afb8)\n",hc,(unsigned long long)s->icnt,pa,g);
    }
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
	    if(idx==0 && getenv("TLBTALLY")) { static uint64_t pv=~0ull; uint64_t c=s->tlb[0].entry_lo1;
	      if(c!=pv){ fprintf(stderr,"[iss-idx0-wr] hi=%llx lo0=%llx lo1=%llx v1=%d icnt=%lu\n",(unsigned long long)s->tlb[0].entry_hi,(unsigned long long)s->tlb[0].entry_lo0,(unsigned long long)c,(int)((c>>1)&1),(unsigned long)s->icnt); pv=c; } }
	    assert(s->tlb[idx].page_mask == 0);
	    iss_tlbdup_check(s, idx, "tlbwi");
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
	    iss_tlbdup_check(s, idx, "tlbwr");
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
	    if(getenv("TLBP_ZERO_ON_MISS")) s->cpr0[CPR0_INDEX] = (1u << 31); /* mimic r9999: index=0 on TLBP miss */
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
	  ptetrack_probe(s, rd, rt);
	  if(rd == CPR0_COMPARE) { /* writing Compare re-arms + clears the timer interrupt (IP[7]) */
	    s->cpr0[CPR0_CAUSE] &= ~(1u << 15);
	    /* EXPERIMENT (env TIMER_SCALE=N): stretch each timer period N-fold by
	     * scaling the just-written interval (Compare-Count), leaving Count itself
	     * untouched so clock calibration (get_r4k_counter) still reads sanely.
	     * Question: how infrequent can timer IRQs get and still boot IRIX? */
	    static const unsigned long g_timer_scale =
	      getenv("TIMER_SCALE") ? strtoul(getenv("TIMER_SCALE"), 0, 0) : 1;
	    if(g_timer_scale > 1) {
	      uint32_t cnt = s->cpr0[CPR0_COUNT];
	      uint32_t interval = s->cpr0[CPR0_COMPARE] - cnt;   /* target - count */
	      /* Only stretch the LARGE periodic scheduler tick (~millions of Count).
	       * get_r4k_counter's clock calibration arms a SHORT interval (~4096) and
	       * spins a bounded countdown waiting for it -- scaling that would break
	       * calibration (the boot cliff at scale>=16).  Leave short intervals alone. */
	      if(interval > 100000u) {
	        s->cpr0[CPR0_COMPARE]    = cnt + interval * (uint32_t)g_timer_scale;
	        s->cpr0_64[CPR0_COMPARE] = s->cpr0[CPR0_COMPARE];
	      }
	    }
	  }
	  /* EXPERIMENT (env MTC0_IRQ_PC=<hexpc>): replicate the r9999-precise
	   * MTC0->Status[IE] CP0 hazard *violation* -- inject a timer IP[7] right
	   * after the mtc0 that writes c0_sr at the given PC, so the timer fires in
	   * the mtc0 shadow (the very next instruction).  If the FPGA bad-istack is
	   * this hazard, the ISS -- which boots past today -- should now panic too. */
	  if(rd == CPR0_SR) {
	    static const char *ip = getenv("MTC0_IRQ_PC");
	    if(ip) {
	      uint32_t pc = (uint32_t)s->pc;
	      uint32_t nsr = s->cpr0[CPR0_SR];
	      /* only inject where the mtc0 actually ENABLES delivery (IE=1,!EXL,!ERL) so
	       * the timer fires in the shadow of an *enable* -- an mtc0 that keeps ints
	       * masked can't fire a shadow irq (that was the SR=0x80 IE=0 miss). */
	      bool enables = (nsr & SR_IE) && !(nsr & (SR_EXL | SR_ERL));
	      bool all  = (ip[0] == 'a');                            /* "all" */
	      bool excl = (pc >= 0x88002200u && pc < 0x88002a00u);   /* int-handler region: skip (storm) */
	      static uint64_t last = 0;
	      bool fire = enables && (all ? (!excl && (s->icnt - last > 400))
	                                  : (pc == (uint32_t)strtoul(ip,0,0)));
	      if(fire) {
	        s->cpr0[CPR0_CAUSE] |= (1u << 15);   /* IP[7] timer pending -> fires next insn */
	        last = s->icnt;
	        static int nf = 0;
	        if(nf++ < 20) fprintf(stderr, "[INJECT] timer IP7 after mtc0 c0_sr @pc=%08x SR=%08x icnt=%llu\n",
	                              pc, s->cpr0[CPR0_SR], (unsigned long long)s->icnt);
	      }
	    }
	  }
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
	  ptetrack_probe(s, rd, rt);
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
      case 0x2f: /* cache */
	{
	  uint32_t rs  = (inst >> 21) & 31;
	  uint32_t cop = (inst >> 16) & 31;       /* [20:18]=op  [17:16]=cache(0=I,1=D,2=SI,3=SD) */
	  int16_t  himm = (int16_t)(inst & 0xffff);
	  uint64_t va  = s->gpr[rs] + (int32_t)himm;
	  { static const bool g_watchinvl = getenv("WATCHINVL") != nullptr;
	    if(g_watchinvl) {
	      uint32_t wpa = 0; bool wm = tlb_probe_ro(s, va, &wpa);
	      if(wm && ((wpa & ~0x1fu) == 0x0086d81c0u)) {
		fprintf(stderr, "[invl] icnt=%llu pc=%08x cache=%u op=%u va=%016llx pa=%08x\n",
			(unsigned long long)s->icnt, (uint32_t)s->pc,
			(unsigned)(cop & 3), (unsigned)((cop >> 2) & 7),
			(unsigned long long)va, wpa);
	      }
	    }
	  }
	  if(g_cache_dbg) {
	    uint32_t dbgpa = 0; bool mapped = tlb_probe_ro(s, va, &dbgpa);
	    fprintf(stderr, "[cache] icnt=%llu pc=%08x op=0x%02x va=%016llx pa=%08x set=%u\n",
		    (unsigned long long)s->icnt, (uint32_t)s->pc, cop,
		    (unsigned long long)va, mapped ? dbgpa : (uint32_t)(va & 0x1fffffff),
		    cache_model::set_of(mapped ? dbgpa : (uint32_t)(va & 0x1fffffff)));
	  }
	  if(g_cmodel) {
	    uint32_t which   = cop & 0x3;         /* 0=I 1=D 2=SI 3=SD */
	    uint32_t cacheop = (cop >> 2) & 0x7;  /* 0=IdxWBInv 4=HitInv 5=HitWBInv 6=HitWB */
	    uint32_t pa = 0;
	    if(which == 1) {                      /* primary D-cache */
	      switch(cacheop) {
	      case 0: g_cmodel->index_wb_inval((uint32_t)va); break;      /* Index_WB_Invalidate */
	      case 4: if(tlb_probe_ro(s, va, &pa)) g_cmodel->hit_inval(pa);    break; /* Hit_Invalidate */
	      case 5: if(tlb_probe_ro(s, va, &pa)) g_cmodel->hit_wb_inval(pa); break; /* Hit_WB_Invalidate */
	      case 6: if(tlb_probe_ro(s, va, &pa)) g_cmodel->hit_wb(pa);       break; /* Hit_WB */
	      default: break;                     /* Index_Load/Store_Tag: no-op (model starts clean) */
	      }
	    }
	    /* I-cache (which==0): D-only model + DRAM-direct fetch already captures code
	     * coherence (a fetch reads DRAM, so the D-cache must be WB'd first).
	     * SD/SI (which 2,3): no secondary cache -> no-op. */
	  }
	  s->pc += 4;
	}
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
