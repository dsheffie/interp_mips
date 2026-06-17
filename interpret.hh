#ifndef __INTERPRET_HH__
#define __INTERPRET_HH__

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <ostream>
#include <cassert>
#include <unordered_map>

#include "sparse_mem.hh"
#include "mips_insns.hh"

#define IS_LITTLE_ENDIAN false

enum class fpMode {
  mipsii, mipsiv, mips32
};

enum class fpOperation {
  abs,neg,mov,add,
  sub,mul,div,sqrt,
  rsqrt,recip,truncl,
  truncw, cvts, cvtd,
  unknown
};

static inline fpOperation decode_fp(uint32_t inst) {
  uint32_t opcode = inst>>26;
  uint32_t functField = (inst>>21) & 31;
  uint32_t lowop = inst & 63;  
  uint32_t fmt = (inst >> 21) & 31;
  uint32_t nd_tf = (inst>>16) & 3;
  uint32_t lowbits = inst & ((1<<11)-1);
  
  if(opcode != 0x11) {
    return fpOperation::unknown;
  }
  opcode &= 0x3;
  if(fmt == 0x8) {
    return fpOperation::unknown;
    }
  else if((lowbits == 0) && ((functField==0x0) || (functField==0x4))) {
    return fpOperation::unknown;    
  }
  if((lowop >> 4) == 3) {
    return fpOperation::unknown;	  
  }
  switch(lowop)
    {
    case 0x0:
      return fpOperation::add;
    case 0x1:
      return fpOperation::sub;
    case 0x2:
      return fpOperation::mul;
    case 0x3:
      return fpOperation::div;
    case 0x4:
      return fpOperation::sqrt;
    case 0x5:
      return fpOperation::abs;
    case 0x6:
      return fpOperation::mov;
    case 0x7:
      return fpOperation::neg;
      //case 0x9:
      //return fpOperation::truncl;
      //case 0xd:
      //return fpOperation::truncw;
    // case 0x11:
    //   _fmovc(inst, s);
    //   break;
    // case 0x12:
    //   _fmovz(inst, s);
    //   break;
    // case 0x13:
    //   _fmovn(inst, s);
    //   break;
    case 0x15:
      return fpOperation::recip;
    case 0x16:
      return fpOperation::rsqrt;
    // case 0x20:
    //   return fpOperation::cvts;
    // case 0x21:
    //   return fpOperation::cvtd;
    default:
      break;
    }
  return fpOperation::unknown;
}

enum class branch_type {
  beq, bne, blez, bgtz,
  beql, bnel, blezl, bgtzl,
  bgez, bgezl, bltz, bltzl,
  bgezal, bltzal, bgezall, bltzall,
  bc1f, bc1t, bc1fl, bc1tl
};


enum class fp_reg_state { unknown, sp, dp };

class sgi_mc;
class sgi_hpc;
class sgi_scc;

class state_t{
public:
  typedef int64_t reg_t;
  reg_t pc = 0;
  reg_t gpr[32] = {0};
  reg_t lo = 0;
  reg_t hi = 0;
  uint32_t cpr0[32] = {0};
  /* FR=1 (mips3/mips4): 32 independent 64-bit FP registers (singles in bits[31:0],
   * doubles use the full 64 bits) -- r9999's FP code reinterpret_casts cpr1+fd. */
  uint64_t cpr1[32] = {0};
  uint32_t fcr1[5] = {0};
  uint64_t icnt = 0;
  uint8_t brk = 0;
  uint64_t maxicnt = 0;
  sparse_mem &mem;
  fp_reg_state cpr1_state[32] = {fp_reg_state::unknown};
  std::unordered_map<mipsInsn,uint64_t> insn_histo;

  /* Software TLB -- mirrors the 48-entry fully-associative RTL TLB */
  static const int NUM_TLB_ENTRIES = 48;
  struct tlb_entry_t {
    uint64_t entry_hi  = 0;   /* R[63:62] + VPN2[39:13] + ASID[7:0] */
    uint64_t entry_lo0 = 0;   /* PFN[33:6] + C[5:3] + D[2] + V[1] + G[0] */
    uint64_t entry_lo1 = 0;
    uint32_t page_mask = 0;   /* variable page size bits [24:13] */
  } tlb[NUM_TLB_ENTRIES];
  /* 64-bit CP0 shadow registers for DMTC0/DMFC0 (EntryHi, EntryLo, XContext) */
  uint64_t cpr0_64[32] = {0};

  /*
   * When true, MMIO side-effects (MTC0 $7 putchar) are suppressed.
   * Set on the checker state so RTL output appears exactly once.
   */
  bool silent = false;
  sgi_mc *mc = nullptr;
  sgi_hpc *hpc = nullptr;
  sgi_scc *scc = nullptr;

  /* True while executing a branch/jump delay-slot instruction.  An exception in
   * a delay slot sets EPC = the branch pc and Cause.BD = 1 (matches the RTL,
   * core.sv: n_epc = in_delay_slot ? pc-4 : pc; n_exc_in_delay = in_delay_slot). */
  bool in_delay_slot = false;

  /* Set by va_translate() when an address translation raised a TLB exception
   * (Refill/Invalid/Modified).  The faulting instruction must abort immediately
   * (do not commit its result or advance pc) -- the exception has already
   * redirected pc to the refill/general vector. Cleared at the top of execMips. */
  bool tlb_fault = false;

  state_t(sparse_mem &mem) : mem(mem) {}
  ~state_t();
};

struct rtype_t {
  uint32_t opcode : 6;
  uint32_t sa : 5;
  uint32_t rd : 5;
  uint32_t rt : 5;
  uint32_t rs : 5;
  uint32_t special : 6;
};

struct itype_t {
  uint32_t imm : 16;
  uint32_t rt : 5;
  uint32_t rs : 5;
  uint32_t opcode : 6;
};

struct coproc1x_t {
  uint32_t fmt : 3;
  uint32_t id : 3;
  uint32_t fd : 5;
  uint32_t fs : 5;
  uint32_t ft : 5;
  uint32_t fr : 5;
  uint32_t opcode : 6;
};

struct lwxc1_t {
  uint32_t id : 6;
  uint32_t fd : 5;
  uint32_t pad : 5;
  uint32_t index : 5;
  uint32_t base : 5;
  uint32_t opcode : 6;
};


union mips_t {
  rtype_t r;
  itype_t i;
  coproc1x_t c1x;
  lwxc1_t lc1x;
  uint32_t raw;
  mips_t(uint32_t x) : raw(x) {}
};


static inline bool is_jr(uint32_t inst, bool r31 = false) {
  uint32_t opcode = inst>>26;
  uint32_t funct = inst & 63;
  uint32_t rs = (inst >> 21) & 31;
  bool jr = (opcode==0) and (funct == 0x08);
  if(jr) {
    return r31 ? (rs==31) : true;
  }
  return false;
}

static inline bool is_jal(uint32_t inst) {
  uint32_t opcode = inst>>26;
  return (opcode == 3);
}

inline bool is_jalr(uint32_t inst) {
  uint32_t opcode = inst>>26;
  return (opcode==0) && ((inst&63) == 0x9);
}


static inline bool is_j(uint32_t inst) {
  uint32_t opcode = inst>>26;
  return (opcode == 2);
}

static inline uint32_t get_jump_target(uint32_t pc, uint32_t inst) {
  assert(is_jal(inst) or is_j(inst));
  static const uint32_t pc_mask = (~((1U<<28)-1));
  uint32_t jaddr = (inst & ((1<<26)-1)) << 2;
  return ((pc + 4)&pc_mask) | jaddr;
}

static inline bool is_memory(uint32_t inst) {
  uint32_t opcode = inst>>26;
  switch(opcode)
    {
    case 0x20: //_lb(inst, s);
    case 0x21: //_lh<EL>(inst, s);
    case 0x22: //_lwl<EL>(inst, s);
    case 0x23: //_lw<EL>(inst, s);
    case 0x24: //_lbu(inst, s);
    case 0x25: //_lhu<EL>(inst, s);
    case 0x26: //_lwr<EL>(inst, s);
    case 0x28: //_sb(inst, s);
    case 0x29: //_sh<EL>(inst, s);
    case 0x2a: //_swl<EL>(inst, s);
    case 0x2b: //_sw<EL>(inst, s);
    case 0x2e: //_swr<EL>(inst, s); 
    case 0x31: //_lwc1<EL>(inst, s);
    case 0x35: //_ldc1<EL>(inst, s);
    case 0x39: //_swc1<EL>(inst, s);
    case 0x3d: //_sdc1<EL>(inst, s);
      return true;
    default:
      break;
    }
  return false;
}

static inline bool is_branch(uint32_t inst) {
  uint32_t opcode = inst>>26;
  uint32_t rt = ((inst>>16) & 31);
  switch(opcode)
    {
    case 0x01:
      return (rt == 0) || (rt == 1);
    case 0x04:
    case 0x05:
    case 0x06:
    case 0x07:
      return true;
    default:
      break;
    }
  return false;
}

static inline bool is_branch_likely(uint32_t inst) {
  uint32_t opcode = inst>>26;
  uint32_t rt = ((inst>>16) & 31);
  switch(opcode)
    {
    case 0x01:
      return (rt == 2) || (rt == 3);
    case 0x14: /* BEQL */
    case 0x15: /* BNEL */
    case 0x16: /* BNEZL */
    case 0x17: /* BGTZL */
      return true;
    default:
      break;
    }
  return false;
}

static inline uint32_t get_branch_target(uint32_t pc, uint32_t inst) {
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
  return  pc+4+imm; 
}

void initState(state_t *s);
void execMips(state_t *s);
void maybe_take_interrupt(state_t *s);  /* CP0 Count/Compare timer tick + Int delivery; call once per step */
void raise_int(state_t *s, uint32_t epc);


std::ostream &operator<<(std::ostream &out, const state_t & s);

bool is_store_insn(state_t *s);

/* CP0 Status register bit positions */
#define SR_IE    (1u <<  0)  /* interrupt enable */
#define SR_EXL   (1u <<  1)  /* exception level */
#define SR_ERL   (1u <<  2)  /* error level */
#define SR_UX    (1u <<  5)  /* user extended (64-bit user mode) */
#define SR_SX    (1u <<  6)  /* supervisor extended */
#define SR_KX    (1u <<  7)  /* kernel extended (64-bit kernel mode) */
#define SR_BEV   (1u << 22)  /* bootstrap exception vectors */
#define SR_CU0   (1u << 28)  /* coprocessor 0 usable */
#define SR_CU1   (1u << 29)  /* coprocessor 1 usable (FPU) */
#define SR_CU2   (1u << 30)  /* coprocessor 2 usable */
#define SR_CU3   (1u << 31)  /* coprocessor 3 usable */

/* CP0 register indices */
#define CPR0_INDEX    0
#define CPR0_RANDOM   1
#define CPR0_ENTRYLO0 2
#define CPR0_ENTRYLO1 3
#define CPR0_CONTEXT  4
#define CPR0_PAGEMASK 5
#define CPR0_WIRED    6
/* 7 = simulator putchar port (MTC0 $rt, $7 outputs rt[7:0]) */
#define CPR0_BADVADDR 8
#define CPR0_COUNT    9
#define CPR0_ENTRYHI  10
#define CPR0_COMPARE  11
#define CPR0_SR       12
#define CPR0_CAUSE    13
#define CPR0_EPC      14
#define CPR0_PRID     15
#define CPR0_CONFIG   16
#define CPR0_XCONTEXT 20
#define CPR0_ERROREPC 30

/* CP0 PRId values (imp field bits [15:8]; R4000 family shares imp 0x04 and is
 * distinguished by the revision byte). */
#define PRID_R4000  0x00000400u   /* imp 0x04, rev 0x00 */
#define PRID_R4400  0x00000440u   /* imp 0x04, rev 0x40 */
#define PRID_R4600  0x00002020u   /* imp 0x20, rev 0x20 -- the SGI Indy CPU */
#define PRID_R10000 0x00000900u   /* imp 0x09, rev 0x00 */
/* The real SGI Indy (and MAME's indy_4610) is an R4600. IRIX's `start` reads
 * PRId and selects per-CPU init (cache config, TLB-refill handler variant,
 * cache-op handling) by the IMP field; presenting R4000 (imp 0x04) makes IRIX
 * run the wrong path. (Confirmed via MAME co-sim, MAME_QUESTIONS.md Q5.) */
#define PRID_VALUE  PRID_R4600

#define VA2PA(x) ((x & 0x1fffffff))

static uint32_t va2pa(uint32_t va) {
  if((va >> 31) & 1) {
    return va & 0x1fffffff;
  }
  return va;
}

#endif
