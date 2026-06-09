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
#include "disassemble.hh"
#include "helper.hh"
#include "globals.hh"

static fpMode currFpMode = fpMode::mipsii;

state_t::~state_t() {
  //std::cout << mem.bytes_allocated() << " bytes present in memory image\n";
  //delete &mem; // mem owned by caller
}

static void execSpecial2(uint32_t inst, state_t *s);
static void execSpecial3(uint32_t inst, state_t *s);
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
  /* Config: return the same constant as the RTL (cache geometry) */
  s->cpr0[CPR0_CONFIG] = 0x00088200;
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

static void raise_adel(state_t *s) {
  s->cpr0[CPR0_EPC]   = (uint32_t)s->pc;
  s->cpr0[CPR0_CAUSE] = (s->cpr0[CPR0_CAUSE] & ~(0x1fu << 2)) | (4u << 2);
  s->cpr0[CPR0_SR]    = (s->cpr0[CPR0_SR] & ~SR_ERL) | SR_EXL;
  s->pc = sext32(0xBFC00180u);
}

static void raise_ades(state_t *s) {
  s->cpr0[CPR0_EPC]   = (uint32_t)s->pc;
  s->cpr0[CPR0_CAUSE] = (s->cpr0[CPR0_CAUSE] & ~(0x1fu << 2)) | (5u << 2);
  s->cpr0[CPR0_SR]    = (s->cpr0[CPR0_SR] & ~SR_ERL) | SR_EXL;
  s->pc = sext32(0xBFC00180u);
}

static void raise_ri(state_t *s, uint32_t inst) {
  fprintf(stderr, "unimplemented: opcode=0x%02x funct=0x%02x @ pc=0x%08x, bits %x\n",
          inst >> 26, inst & 0x3fu, (uint32_t)s->pc, inst);
  s->cpr0[CPR0_EPC]   = (uint32_t)s->pc;
  s->cpr0[CPR0_CAUSE] = (s->cpr0[CPR0_CAUSE] & ~(0x1fu << 2)) | (10u << 2);
  s->cpr0[CPR0_SR]    = (s->cpr0[CPR0_SR] & ~SR_ERL) | SR_EXL;
  s->pc = sext32(0xBFC00180u);
}

static void raise_overflow(state_t *s) {
  s->cpr0[CPR0_EPC]   = (uint32_t)s->pc;
  s->cpr0[CPR0_CAUSE] = (s->cpr0[CPR0_CAUSE] & ~(0x1fu << 2)) | (12u << 2);
  s->cpr0[CPR0_SR]    = (s->cpr0[CPR0_SR] & ~SR_ERL) | SR_EXL;
  s->pc = sext32(0xBFC00180u);
}

static void raise_trap(state_t *s) {
  s->cpr0[CPR0_EPC]   = (uint32_t)s->pc;
  s->cpr0[CPR0_CAUSE] = (s->cpr0[CPR0_CAUSE] & ~(0x1fu << 2)) | (13u << 2);
  s->cpr0[CPR0_SR]    = (s->cpr0[CPR0_SR] & ~SR_ERL) | SR_EXL;
  s->pc = sext32(0xBFC00180u);
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



static void execSpecial2(uint32_t inst,state_t *s) {
  uint32_t funct = inst & 63; 
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;

  switch(funct)
    {
    case(0x0): /* madd */ {
      int64_t y,acc;
      acc = ((int64_t)(uint32_t)s->hi) << 32;
      acc |= (uint32_t)s->lo;
      y = (int64_t)s->gpr[rs] * (int64_t)s->gpr[rt];
      y += acc;
      s->lo = (int32_t)(y & 0xffffffff);
      s->hi = (int32_t)(y >> 32);
      s->insn_histo[mipsInsn::MADD]++;
      break;
    }
    case 0x1: /* maddu */ {
      uint64_t y,acc;
      uint64_t uk0 = (uint64_t)(uint32_t)s->gpr[rs];
      uint64_t uk1 = (uint64_t)(uint32_t)s->gpr[rt];
      y = uk0*uk1;
      acc = ((uint64_t)(uint32_t)s->hi) << 32;
      acc |= (uint64_t)(uint32_t)s->lo;
      y += acc;
      s->lo = sext64((uint32_t)(y & 0xffffffff));
      s->hi = sext64((uint32_t)(y >> 32));
      s->insn_histo[mipsInsn::MADDU]++;
      break;
    }
    case(0x2): /* mul */{
      int64_t y = ((int64_t)s->gpr[rs]) * ((int64_t)s->gpr[rt]);
      //printf("multiply: %x x %x -> %x\n", s->gpr[rs], s->gpr[rt], y);
      s->gpr[rd] = (int32_t)y;
      s->insn_histo[mipsInsn::MUL]++;
      break;
    }
    case(0x4): /* msub */ {
      int64_t y,acc;
      acc = ((int64_t)s->hi) << 32;
      acc |= ((int64_t)s->lo);
      y = (int64_t)s->gpr[rs] * (int64_t)s->gpr[rt];
      y = acc - y;
      s->lo = (int32_t)(y & 0xffffffff);
      s->hi = (int32_t)(y >> 32);
      s->insn_histo[mipsInsn::MSUB]++;
      break;
    }
    case(0x20): /* clz */
      s->gpr[rd] = (s->gpr[rs]==0) ? 32 : __builtin_clz(s->gpr[rs]);
      s->insn_histo[mipsInsn::CLZ]++;
      break;
    default:
      printf("unhandled special2 instruction @ 0x%08x\n", s->pc); 
      exit(-1);
      break;
    }
  s->pc += 4;
}

static void execSpecial3(uint32_t inst,state_t *s) {
  uint32_t funct = inst & 63;
  uint32_t op = (inst>>6) & 31;
  uint32_t rt = (inst >> 16) & 31; 
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rd = (inst >> 11) & 31;
  if(funct == 32) {
    switch(op)
      {
      case 0x10: /* seb */
	s->gpr[rd] = (int32_t)((int8_t)s->gpr[rt]);
	s->insn_histo[mipsInsn::SEB]++;
	break;
      case 0x18: /* seh */
	s->gpr[rd] = (int32_t)((int16_t)s->gpr[rt]);
	s->insn_histo[mipsInsn::SEH]++;
	break;
      default:
	printf("unhandled special3 instruction @ 0x%08x, opcode = %x\n", s->pc, funct); 
	exit(-1);    
	break;
      }
  }
  else if(funct == 0) { /* ext */  
    uint32_t pos = (inst >> 6) & 31;
    uint32_t size = ((inst >> 11) & 31) + 1;
    s->gpr[rt] = (s->gpr[rs] >> pos) & ((1<<size)-1);
    s->insn_histo[mipsInsn::EXT]++;
  }
  else if(funct == 0x4) {/* ins */
    uint32_t size = rd-op+1;
    uint32_t mask = (1U<<size) -1;
    uint32_t cmask = ~(mask << op);
    uint32_t v = (s->gpr[rs] & mask) << op;
    uint32_t c = (s->gpr[rt] & cmask) | v;
    s->gpr[rt] = c;
    s->insn_histo[mipsInsn::INS]++;    
  }
  else if(funct == 0x3b) { /* rdhwr */
    switch(rd)
      {
      case 29:
	s->gpr[rt] = s->cpr0[29];
	s->insn_histo[mipsInsn::RDHWR]++;
	break;
      default:
	abort();
      }
  }
  else {
    printf("unhandled special3 instruction @ 0x%08x\n", s->pc); 
    exit(-1);    
  }
  s->pc += 4;
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
  uint32_t ea = va2pa(s->gpr[mi.lc1x.base] + s->gpr[mi.lc1x.index]);
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
      s->insn_histo[mipsInsn::BEQL]++;
      isLikely = true;
      break;
    case branch_type::beq:
      takeBranch = (s->gpr[rt] == s->gpr[rs]);
      s->insn_histo[mipsInsn::BEQ]++;
      break;
    case branch_type::bnel:
      isLikely = true;
      takeBranch = (s->gpr[rt] != s->gpr[rs]);
      s->insn_histo[mipsInsn::BNEL]++;
      break;
    case branch_type::bne:
      takeBranch = (s->gpr[rt] != s->gpr[rs]);
      s->insn_histo[mipsInsn::BNE]++;
      break;
    case branch_type::blezl:
      isLikely = true;
      takeBranch = (s->gpr[rs] <= 0);
      s->insn_histo[mipsInsn::BLEZL]++;
      break;
    case branch_type::blez:
      takeBranch = (s->gpr[rs] <= 0);
      s->insn_histo[mipsInsn::BLEZ]++;
      break;
    case branch_type::bgtzl:
      isLikely = true;
      takeBranch = (s->gpr[rs] > 0);
      s->insn_histo[mipsInsn::BGTZL]++;
      break;
    case branch_type::bgtz:
      takeBranch = (s->gpr[rs] > 0);
      s->insn_histo[mipsInsn::BGTZ]++;
      break;
    case branch_type::bgezl:
      isLikely = true;
      takeBranch = (s->gpr[rs] >= 0);
      s->insn_histo[mipsInsn::BGEZL]++;
      break;      
    case branch_type::bgez:
      takeBranch = (s->gpr[rs] >= 0);
      s->insn_histo[mipsInsn::BGEZ]++;
      break;
    case branch_type::bltzl:
      isLikely = true;
      takeBranch = (s->gpr[rs] < 0);
      s->insn_histo[mipsInsn::BLTZL]++;
      break;
    case branch_type::bltz:
      takeBranch = (s->gpr[rs] < 0);
      s->insn_histo[mipsInsn::BLTZ]++;
      break;
    case branch_type::bgezal:
      takeBranch = (s->gpr[rs] >= 0);
      s->insn_histo[mipsInsn::BGEZAL]++;
      saveReturn = true;
      break;
    case branch_type::bc1tl:
      isLikely = true;
      takeBranch = getConditionCode(s,((inst>>18)&7))==1;
      s->insn_histo[mipsInsn::BC1TL]++;
      break;
    case branch_type::bc1t:
      takeBranch = getConditionCode(s,((inst>>18)&7))==1;
      s->insn_histo[mipsInsn::BC1T]++;
      break;
    case branch_type::bc1fl:
      isLikely = true;
      takeBranch = getConditionCode(s,((inst>>18)&7))==0;
      s->insn_histo[mipsInsn::BC1FL]++;
      break;
    case branch_type::bc1f:
      takeBranch = getConditionCode(s,((inst>>18)&7))==0;
      s->insn_histo[mipsInsn::BC1F]++;
      break;
    default:
      UNREACHABLE();
    }

  s->pc += 4;
  if(isLikely) {
    if(takeBranch) {
      execMips<EL>(s);
      s->pc = (imm+npc);
    }
    else {
      s->pc += 4;
    }
  }
  else {
    execMips<EL>(s);
    if(takeBranch){
      if(saveReturn) {
	s->gpr[31] = sext32((uint32_t)(npc + 4));
      }
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
  uint32_t ea = va2pa(s->gpr[rs] + imm);
  if(ea & 3) { raise_adel(s); return; }
  s->gpr[rt] = bswap<EL>(s->mem.get<int32_t>(ea));
  //#define TRACE_MEM
  //printf("_lw pc %x from ea %x = %x\n", s->pc, (uint32_t)s->gpr[rs] + imm,
  //s->gpr[rt]);
  //#undef TRACE_MEM
  s->pc += 4;
  s->insn_histo[mipsInsn::LW]++;
}

template <bool EL>
void _lh(uint32_t inst, state_t *s) {
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;

  uint32_t ea = va2pa(s->gpr[rs] + imm);
  if(ea & 1) { raise_adel(s); return; }
  int16_t mem = bswap<EL>(s->mem.get<int16_t>(ea));
  
  s->gpr[rt] = static_cast<int32_t>(mem);
#ifdef TRACE_MEM
  printf("_lh from %x = %x\n", ea, s->gpr[rt]);
#endif
  s->pc +=4;
  s->insn_histo[mipsInsn::LH]++;  
}


static void _lb(uint32_t inst, state_t *s){
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  
  uint32_t ea = va2pa(s->gpr[rs] + imm);
  s->gpr[rt] = static_cast<int32_t>(s->mem.get<int8_t>(ea));
#ifdef TRACE_MEM
  printf("_lb from %x = %x\n", ea, s->gpr[rt]);
#endif  
  s->pc += 4;
  s->insn_histo[mipsInsn::LB]++;  
}

static void _lbu(uint32_t inst, state_t *s){
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;

  uint32_t ea = va2pa(s->gpr[rs] + imm);
  uint32_t zExt = s->mem.get<uint8_t>(ea);
  *((uint64_t*)&(s->gpr[rt])) = zExt;
  s->pc += 4;
  s->insn_histo[mipsInsn::LBU]++;
}


template <bool EL>
void _lhu(uint32_t inst, state_t *s) {
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;

  uint32_t ea = va2pa(s->gpr[rs] + imm);
  if(ea & 1) { raise_adel(s); return; }
  uint32_t zExt = bswap<EL>(s->mem.get<uint16_t>(ea));
  *((uint64_t*)&(s->gpr[rt])) = zExt;
  //printf("_lhu from %x = %x\n", ea, s->gpr[rt]);  
  s->pc += 4;
  s->insn_histo[mipsInsn::LHU]++;  
}


template <bool EL>
void _sw(uint32_t inst, state_t *s) {
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  uint32_t ea = va2pa(s->gpr[rs] + imm);
  if(ea & 3) { raise_ades(s); return; }
  s->mem.set<int32_t>(ea,  bswap<EL>(static_cast<int32_t>(s->gpr[rt])));
  s->pc += 4;
  s->insn_histo[mipsInsn::SW]++;
}

template <bool EL>
void _sd(uint32_t inst, state_t *s) {
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int32_t imm = (int32_t)(int16_t)(inst & 0xffffu);
  uint32_t ea = va2pa(s->gpr[rs] + imm);
  if(ea & 7) { raise_ades(s); return; }
  s->mem.set<int64_t>(ea, bswap<EL>(s->gpr[rt]));
  s->pc += 4;
  s->insn_histo[mipsInsn::SD]++;
}

template <bool EL>
void _ld(uint32_t inst, state_t *s) {
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int32_t imm = (int32_t)(int16_t)(inst & 0xffffu);
  uint32_t ea = va2pa(s->gpr[rs] + imm);
  if(ea & 7) { raise_adel(s); return; }
  s->gpr[rt] = bswap<EL>(s->mem.get<int64_t>(ea));
  s->pc += 4;
  s->insn_histo[mipsInsn::LD]++;
}

template <bool EL>
void _sc(uint32_t inst, state_t *s) {
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  uint32_t ea = va2pa(s->gpr[rs] + imm);
  s->mem.set<int32_t>(ea,  bswap<EL>(static_cast<int32_t>(s->gpr[rt])));
  s->gpr[rt] = 1;
  s->pc += 4;
  s->insn_histo[mipsInsn::SC]++;
}

template <bool EL>
void _scd(uint32_t inst, state_t *s) {
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int32_t imm = (int32_t)(int16_t)(inst & 0xffffu);
  uint32_t ea = va2pa(s->gpr[rs] + imm);
  s->mem.set<int64_t>(ea, bswap<EL>(static_cast<int64_t>(s->gpr[rt])));
  s->gpr[rt] = 1;
  s->pc += 4;
  s->insn_histo[mipsInsn::SC]++;
}


template <bool EL>
void _sh(uint32_t inst, state_t *s) {
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
    
  uint32_t ea = va2pa(s->gpr[rs] + imm);
  if(ea & 1) { raise_ades(s); return; }
  s->mem.set<int16_t>(ea,  bswap<EL>(((int16_t)s->gpr[rt])));
  s->pc += 4;
  s->insn_histo[mipsInsn::SH]++;
}

static void _sb(uint32_t inst, state_t *s) {
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
    
  uint32_t ea = va2pa(s->gpr[rs] + imm);
  s->mem.set<uint8_t>(ea, static_cast<uint8_t>(s->gpr[rt]));
  
  s->pc +=4;
  s->insn_histo[mipsInsn::SB]++;
}

static void _mtc1(uint32_t inst, state_t *s) {
  uint32_t rd = (inst>>11) & 31;
  uint32_t rt = (inst>>16) & 31;
  s->cpr1[rd] = s->gpr[rt];
  s->pc += 4;
  s->insn_histo[mipsInsn::MTC1]++;  
}

static void _mfc1(uint32_t inst, state_t *s) {
  uint32_t rd = (inst>>11) & 31;
  uint32_t rt = (inst>>16) & 31;
  s->gpr[rt] = s->cpr1[rd];
  s->pc +=4;
  s->insn_histo[mipsInsn::MFC1]++;
}


template <bool EL>
void _swl(uint32_t inst, state_t *s) {
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  uint32_t ea = va2pa(s->gpr[rs] + imm);
  uint32_t ma = ea & 3;
  ea &= 0xfffffffc;
  if(EL)
    ma = 3 - ma;
  uint32_t r = bswap<EL>(s->mem.get<uint32_t>(ea));   
  uint32_t xx=0,x = s->gpr[rt];
  
  uint32_t xs = x >> (8*ma);
  uint32_t m = ~((1U << (8*(4 - ma))) - 1);
  xx = (r & m) | xs;
  //std::cout << "SIM SWL EA " << std::hex << ea
  //<< ", MA = " << ma
  //<< ", X = " << x
  //<< ", R = " << r
  //<< ", M = " << m
  //	    << ", XX = " << xx << std::dec << "\n";
  
  s->mem.set<uint32_t>(ea, bswap<EL>(xx));
  s->pc += 4;
  s->insn_histo[mipsInsn::SWL]++;  
}

template <bool EL>
void _swr(uint32_t inst, state_t *s) {
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  uint32_t ea = va2pa(s->gpr[rs] + imm);
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
  s->insn_histo[mipsInsn::SWR]++;
}

template <bool EL>
void _lwl(uint32_t inst, state_t *s) {
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  
  uint32_t ea = va2pa((uint32_t)s->gpr[rs] + imm);
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
  s->insn_histo[mipsInsn::LWL]++;  
}

template<bool EL>
void _lwr(uint32_t inst, state_t *s) {
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
 
  uint32_t ea = va2pa((uint32_t)s->gpr[rs] + imm);
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
  s->insn_histo[mipsInsn::LWR]++;
}

template <bool EL>
void _ldl(uint32_t inst, state_t *s) {
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int32_t imm = (int32_t)(int16_t)(inst & 0xffff);
  uint32_t ea = va2pa(s->gpr[rs] + imm);
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
  s->insn_histo[mipsInsn::LDL]++;
}

template <bool EL>
void _ldr(uint32_t inst, state_t *s) {
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int32_t imm = (int32_t)(int16_t)(inst & 0xffff);
  uint32_t ea = va2pa(s->gpr[rs] + imm);
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
  s->insn_histo[mipsInsn::LDR]++;
}

template <bool EL>
void _sdl(uint32_t inst, state_t *s) {
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int32_t imm = (int32_t)(int16_t)(inst & 0xffff);
  uint32_t ea = va2pa(s->gpr[rs] + imm);
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
  s->insn_histo[mipsInsn::SDL]++;
}

template <bool EL>
void _sdr(uint32_t inst, state_t *s) {
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int32_t imm = (int32_t)(int16_t)(inst & 0xffff);
  uint32_t ea = va2pa(s->gpr[rs] + imm);
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
  s->insn_histo[mipsInsn::SDR]++;
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
  uint32_t ea = va2pa(s->gpr[rs] + imm);
  *reinterpret_cast<int64_t*>(s->cpr1 + ft) = bswap<EL>(s->mem.get<int64_t>(ea));
  s->pc += 4;
  s->insn_histo[mipsInsn::LDC1]++;
}

template <bool EL>
void _sdc1(uint32_t inst, state_t *s) {
  uint32_t ft = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  uint32_t ea = va2pa(s->gpr[rs] + imm);
  s->mem.set<int64_t>(ea,  bswap<EL>((*(int64_t*)(s->cpr1 + ft))));
  s->pc += 4;
  s->insn_histo[mipsInsn::SDC1]++;  
}

template <bool EL>
void _lwc1(uint32_t inst, state_t *s) {
  uint32_t ft = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  uint32_t ea = va2pa(s->gpr[rs] + imm);
  uint32_t v = bswap<EL>(s->mem.get<uint32_t>(ea)); 
  *((float*)(s->cpr1 + ft)) = *((float*)&v);
  s->pc += 4;
  s->insn_histo[mipsInsn::LWC1]++;
}

template <bool EL>
void _swc1(uint32_t inst, state_t *s) {
  uint32_t ft = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  uint32_t ea = va2pa(s->gpr[rs] + imm);
  uint32_t v = *((uint32_t*)(s->cpr1+ft));
  s->mem.set<uint32_t>(ea, bswap<EL>(v));
  s->pc += 4;
  s->insn_histo[mipsInsn::SWC1]++;
}

static void _truncl(uint32_t inst, state_t *s) {
  printf("%s\n",__func__);
  exit(-1);
}

static void _truncw(uint32_t inst, state_t *s) {
  uint32_t fmt = (inst >> 21) & 31;
  uint32_t fd = (inst>>6) & 31;
  uint32_t fs = (inst>>11) & 31;
  float f;
  double d;
  int32_t *ptr = ((int32_t*)(s->cpr1 + fd));
  if(currFpMode != fpMode::mips32) {
    assert((fd & 1) == 0);
    assert((fs & 1) == 0);
  }  
  switch(fmt)
    {
    case FMT_S:
      f = (*((float*)(s->cpr1 + fs)));
      //printf("f=%g\n", f);
      *ptr = (int32_t)f;
      s->insn_histo[mipsInsn::TRUNC_SP_W]++;
      break;
    case FMT_D:
      d = (*((double*)(s->cpr1 + fs)));
      *ptr = (int32_t)d;
      s->insn_histo[mipsInsn::TRUNC_DP_W]++;      
      //printf("id=%d\n", *ptr);
      break;
    default:
      printf("unknown trunc for fmt %d\n", fmt);
      exit(-1);
      break;
    }
  if(currFpMode != fpMode::mips32) {
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
      s->cpr1[fd+1] = s->cpr1[fs+1];
    }
    s->insn_histo[mipsInsn::FP_MOVF];    
  }
  else {
    if(getConditionCode(s,cc)==1) {
      s->cpr1[fd+0] = s->cpr1[fs+0];
      s->cpr1[fd+1] = s->cpr1[fs+1];
    }
    s->insn_histo[mipsInsn::FP_MOVT];    
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
    s->insn_histo[mipsInsn::FP_MOVF];
  }
  else {
    s->cpr1[fd+0] = getConditionCode(s, cc) ? s->cpr1[fs+0] : s->cpr1[fd+0];
    s->insn_histo[mipsInsn::FP_MOVT];
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
    s->insn_histo[mipsInsn::MOVF]++;
  }
  else {
    /* movt */
    s->gpr[rd] = getConditionCode(s, cc) ? s->gpr[rs] : s->gpr[rd];
    s->insn_histo[mipsInsn::MOVT]++;    
  }
  s->pc += 4;
}

static void _cvts(uint32_t inst, state_t *s) {
  uint32_t fmt = (inst >> 21) & 31;
  uint32_t fd = (inst>>6) & 31;
  uint32_t fs = (inst>>11) & 31;
  if(currFpMode != fpMode::mips32) {
    assert((fd & 1) == 0);
    assert((fs & 1) == 0);
  }
  switch(fmt)
    {
    case FMT_D:
      *((float*)(s->cpr1 + fd)) = (float)(*((double*)(s->cpr1 + fs)));
      if(currFpMode != fpMode::mips32) {
	s->cpr1[fd+1] = 0;
      }
      s->cpr1_state[fd] = fp_reg_state::sp;      
      break;
    case FMT_W:
      *((float*)(s->cpr1 + fd)) = (float)(*((int32_t*)(s->cpr1 + fs)));
      if(currFpMode != fpMode::mips32) {
	*((float*)(s->cpr1 + fd + 1)) = 0;
      }
      break;
    default:
      printf("%s @ %d\n", __func__, __LINE__);
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
    default:
      printf("%s @ %d\n", __func__, __LINE__);
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
      s->insn_histo[select_fp_insn<T>(mipsInsn::DP_CMP_EQ, mipsInsn::SP_CMP_EQ)]++;            
      s->fcr1[CP1_CR25] = setBit(s->fcr1[CP1_CR25],v,cc);
      break;
    case COND_LT:
      v = (Tfs < Tft);
      s->insn_histo[select_fp_insn<T>(mipsInsn::DP_CMP_LT, mipsInsn::SP_CMP_LT)]++;      
      s->fcr1[CP1_CR25] = setBit(s->fcr1[CP1_CR25],v,cc);
      break;
    case COND_LE:
      v = (Tfs <= Tft);
      s->insn_histo[select_fp_insn<T>(mipsInsn::DP_CMP_LE, mipsInsn::SP_CMP_LE)]++;            
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
      s->insn_histo[select_fp_insn<T>(mipsInsn::DP_ABS, mipsInsn::SP_ABS)]++;      
      break;
    case fpOperation::neg:
      _fd = -_fs;
      s->insn_histo[select_fp_insn<T>(mipsInsn::DP_NEG, mipsInsn::SP_NEG)]++;
      break;
    case fpOperation::mov:
      _fd = _fs;
      s->insn_histo[select_fp_insn<T>(mipsInsn::DP_MOV, mipsInsn::SP_MOV)]++;            
      break;
    case fpOperation::add:
      _fd = _fs + _ft;
      s->insn_histo[select_fp_insn<T>(mipsInsn::DP_ADD, mipsInsn::SP_ADD)]++;            
      break;
    case fpOperation::sub:
      _fd = _fs - _ft;
      s->insn_histo[select_fp_insn<T>(mipsInsn::DP_SUB, mipsInsn::SP_SUB)]++;                  
      break;
    case fpOperation::mul:
      _fd = _fs * _ft;
      s->insn_histo[select_fp_insn<T>(mipsInsn::DP_MUL, mipsInsn::SP_MUL)]++;      
      break;
    case fpOperation::div:
      if(_ft==0.0) {
	_fd = std::numeric_limits<T>::max();
      }
      else {
	_fd = _fs / _ft;
      }
      s->insn_histo[select_fp_insn<T>(mipsInsn::DP_DIV, mipsInsn::SP_DIV)]++;       
      break;
    case fpOperation::sqrt:
      _fd = std::sqrt(_fs);
      s->insn_histo[select_fp_insn<T>(mipsInsn::DP_SQRT, mipsInsn::SP_SQRT)]++;      
      break;
    case fpOperation::rsqrt:
      _fd = static_cast<T>(1.0) / std::sqrt(_fs);
      s->insn_histo[select_fp_insn<T>(mipsInsn::DP_RSQRT, mipsInsn::SP_RSQRT)]++;
      break;
    case fpOperation::recip:
      _fd = static_cast<T>(1.0) / _fs;
      s->insn_histo[select_fp_insn<T>(mipsInsn::DP_RECIP, mipsInsn::SP_RECIP)]++;
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
    assert((fd&1) == 0);
    execFP<float,op>(inst,s);
    s->cpr1[fd+1] = 0;
    s->cpr1_state[fd] = fp_reg_state::sp;
    s->cpr1_state[fd+1] = fp_reg_state::unknown;
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
  else if((lowbits == 0) && ((functField==0x0) || (functField==0x4)))
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
  uint32_t inst = bswap<EL>(mem.get<uint32_t>(va2pa(s->pc)));

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
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  s->icnt++;
    
  if(isRType) {
    uint32_t funct = inst & 63;
    uint32_t sa = (inst >> 6) & 31;
    switch(funct) 
      {
      case 0x00: /*sll*/
	s->gpr[rd] = static_cast<int32_t>(s->gpr[rt]) << sa;
	s->pc += 4;
	if(inst == 0) {
	  s->insn_histo[mipsInsn::NOP]++;
	}
	else {
	  s->insn_histo[mipsInsn::SLL]++;
	}
	break;
      case 0x01: /* movci */
	_movci(inst,s);
	break;
      case 0x02: /* srl */
	s->gpr[rd] = sext64(((uint32_t)s->gpr[rt] >> sa));
	s->pc += 4;
	s->insn_histo[mipsInsn::SRL]++;
	break;
      case 0x03: /* sra */
	s->gpr[rd] = static_cast<int32_t>(s->gpr[rt]) >> sa;
	s->pc += 4;
	s->insn_histo[mipsInsn::SRA]++;
	break;	
      case 0x04: /* sllv */
	s->gpr[rd] = sext64(static_cast<uint32_t>(s->gpr[rt]) << (s->gpr[rs] & 0x1f));
	s->pc += 4;
	s->insn_histo[mipsInsn::SLLV]++;
	break;
      case 0x06:  
	s->gpr[rd] = ((uint32_t)s->gpr[rt]) >> (s->gpr[rs] & 0x1f);
	s->pc += 4;
	s->insn_histo[mipsInsn::SRLV]++;
	break;
      case 0x07:  
	s->gpr[rd] = s->gpr[rt] >> (s->gpr[rs] & 0x1f);
	s->pc += 4;
	s->insn_histo[mipsInsn::SRAV]++;
	break;
      case 0x08: { /* jr */
	state_t::reg_t jaddr = s->gpr[rs];
	s->pc += 4;
	execMips<EL>(s);
	s->pc = jaddr;
	s->insn_histo[mipsInsn::JR]++;	
	break;
      }
      case 0x09: { /* jalr */
	state_t::reg_t jaddr = s->gpr[rs];
	s->gpr[31] = sext32((uint32_t)(s->pc + 8));
	s->pc += 4;
	execMips<EL>(s);
	s->pc = jaddr;
	s->insn_histo[mipsInsn::JALR]++;	
	break;
      }
      case 0x0C: /* syscall */
      case 0x0D: /* break */
	s->brk = 1;
	s->pc += 4;    /* advance so the checker stays in sync */
	if(!s->silent) {
	  std::cout << "got break or syscall\n";
	}
	s->insn_histo[mipsInsn::BREAK]++;
	break;
      case 0x0f: /* sync */
	s->pc += 4;
	s->insn_histo[mipsInsn::SYNC]++;
	break;
      case 0x10: /* mfhi */
	s->gpr[rd] = s->hi;
	s->pc += 4;
	s->insn_histo[mipsInsn::MFHI]++;
	break;
      case 0x11: /* mthi */ 
	s->hi = s->gpr[rs];
	s->pc += 4;
	s->insn_histo[mipsInsn::MTHI]++;
	break;
      case 0x12: /* mflo */
	s->gpr[rd] = s->lo;
	s->pc += 4;
	s->insn_histo[mipsInsn::MFLO]++;	
	break;
      case 0x13: /* mtlo */
	s->lo = s->gpr[rs];
	s->pc += 4;
	s->insn_histo[mipsInsn::MTLO]++;		
	break;
      case 0x18: { /* mult */
	int64_t y;
	y = (int64_t)s->gpr[rs] * (int64_t)s->gpr[rt];
	s->lo = (int32_t)(y & 0xffffffff);
	s->hi = (int32_t)(y >> 32);
	s->pc += 4;
	s->insn_histo[mipsInsn::MULT]++;			
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
	s->insn_histo[mipsInsn::MULTU]++;
	break;
      }
      case 0x1A: /* div */
	if(s->gpr[rt] != 0) {
	  s->lo = s->gpr[rs] / s->gpr[rt];
	  s->hi = s->gpr[rs] % s->gpr[rt];
	}
	s->pc += 4;
	s->insn_histo[mipsInsn::DIV]++;
	break;
      case 0x1B: /* divu */
	if(s->gpr[rt] != 0) {
	  s->lo = sext64((uint32_t)s->gpr[rs] / (uint32_t)s->gpr[rt]);
	  s->hi = sext64((uint32_t)s->gpr[rs] % (uint32_t)s->gpr[rt]);
	}
	s->pc += 4;
	s->insn_histo[mipsInsn::DIVU]++;
	break;
      case 0x1C: { /* dmult: signed 64x64 -> 128, hi:lo */
	__int128 y = (__int128)s->gpr[rs] * (__int128)s->gpr[rt];
	s->lo = (int64_t)(y & 0xffffffffffffffffULL);
	s->hi = (int64_t)(y >> 64);
	s->pc += 4;
	s->insn_histo[mipsInsn::DMULT]++;
	break;
      }
      case 0x1D: { /* dmultu: unsigned 64x64 -> 128, hi:lo */
	unsigned __int128 y = (unsigned __int128)(uint64_t)s->gpr[rs]
	                    * (unsigned __int128)(uint64_t)s->gpr[rt];
	s->lo = (int64_t)(y & 0xffffffffffffffffULL);
	s->hi = (int64_t)(uint64_t)(y >> 64);
	s->pc += 4;
	s->insn_histo[mipsInsn::DMULTU]++;
	break;
      }
      case 0x1E: /* ddiv: signed 64-bit divide */
	if(s->gpr[rt] != 0) {
	  s->lo = s->gpr[rs] / s->gpr[rt];
	  s->hi = s->gpr[rs] % s->gpr[rt];
	}
	s->pc += 4;
	s->insn_histo[mipsInsn::DDIV]++;
	break;
      case 0x1F: /* ddivu: unsigned 64-bit divide */
	if(s->gpr[rt] != 0) {
	  s->lo = (int64_t)((uint64_t)s->gpr[rs] / (uint64_t)s->gpr[rt]);
	  s->hi = (int64_t)((uint64_t)s->gpr[rs] % (uint64_t)s->gpr[rt]);
	}
	s->pc += 4;
	s->insn_histo[mipsInsn::DDIVU]++;
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
	s->insn_histo[mipsInsn::ADD]++;
	break;
      }
      case 0x21: { /* addu */
	uint32_t u_rs = (uint32_t)s->gpr[rs];
	uint32_t u_rt = (uint32_t)s->gpr[rt];
	s->gpr[rd] = sext64(u_rs + u_rt);
	s->pc += 4;
	s->insn_histo[mipsInsn::ADDU]++;
	break;
      }
      case 0x22: { /* sub */
	uint32_t u_rs = (uint32_t)s->gpr[rs];
	uint32_t u_rt = (uint32_t)s->gpr[rt];
	uint32_t result = u_rs - u_rt;
	/* Overflow iff operands have different signs AND result sign != rs sign.
	 * Matches RTL: w_sub32_overflow = (result[31]!=rt[31]) & (rs[31]!=rt[31]) */
	if (((result >> 31) != (u_rt >> 31)) && ((u_rs >> 31) != (u_rt >> 31))) {
	  raise_overflow(s);
	  break;
	}
	s->gpr[rd] = sext64(result);
	s->pc += 4;
	s->insn_histo[mipsInsn::SUB]++;
	break;
      }
      case 0x23:{ /*subu*/  
	uint32_t u_rs = (uint32_t)s->gpr[rs];
	uint32_t u_rt = (uint32_t)s->gpr[rt];
	uint32_t y = u_rs - u_rt;
	s->gpr[rd] = sext64(y);
	s->pc += 4;
	s->insn_histo[mipsInsn::SUBU]++;
	break;
      }
      case 0x24: /* and */
	s->gpr[rd] = s->gpr[rs] & s->gpr[rt];
	s->pc += 4;
	s->insn_histo[mipsInsn::AND]++;
	break;
      case 0x25: /* or */
	if(rd != 0) {
	  s->gpr[rd] = s->gpr[rs] | s->gpr[rt];
	}
	s->pc += 4;
	s->insn_histo[mipsInsn::OR]++;
	break;
      case 0x26: /* xor */
	s->gpr[rd] = s->gpr[rs] ^ s->gpr[rt];
	s->pc += 4;
	s->insn_histo[mipsInsn::XOR]++;	
	break;
      case 0x27: /* nor */
	s->gpr[rd] = ~(s->gpr[rs] | s->gpr[rt]);
	s->pc += 4;
	s->insn_histo[mipsInsn::NOR]++;
	break;
      case 0x2A: /* slt */
	s->gpr[rd] = s->gpr[rs] < s->gpr[rt];
	s->pc += 4;
	s->insn_histo[mipsInsn::SLT]++;	
	break;
      case 0x2B: { /* sltu */
	s->gpr[rd] = ((uint64_t)s->gpr[rs] < (uint64_t)s->gpr[rt]);
	s->pc += 4;
	s->insn_histo[mipsInsn::SLTU]++;
	break;
      }
      case 0x0B: /* movn */
	s->gpr[rd] = (s->gpr[rt] != 0) ? s->gpr[rs] : s->gpr[rd];
	s->pc +=4;
	s->insn_histo[mipsInsn::MOVN]++;
	break;
      case 0x0A: /* movz */
	s->gpr[rd] = (s->gpr[rt] == 0) ? s->gpr[rs] : s->gpr[rd];
	s->pc += 4;
	s->insn_histo[mipsInsn::MOVZ]++;	
	break;
      case 0x34: /* teq */
	if(s->gpr[rs] == s->gpr[rt]) {
	  raise_trap(s);
	  return;
	}
	s->pc += 4;
	s->insn_histo[mipsInsn::TEQ]++;
	break;
      case 0x36: /* tne */
	if(s->gpr[rs] != s->gpr[rt]) {
	  raise_trap(s);
	  return;
	}
	s->pc += 4;
	s->insn_histo[mipsInsn::TNE]++;
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
	s->insn_histo[mipsInsn::DADD]++;
	break;
      }
      case 0x2D: /* daddu */
	s->gpr[rd] = s->gpr[rs] + s->gpr[rt];
	s->pc += 4;
	s->insn_histo[mipsInsn::DADDU]++;
	break;
      case 0x2E: { /* dsub */
	uint64_t u_rs = (uint64_t)s->gpr[rs];
	uint64_t u_rt = (uint64_t)s->gpr[rt];
	uint64_t result = u_rs - u_rt;
	/* Matches RTL: w_sub64_overflow = (result[63]!=rt[63]) & (rs[63]!=rt[63]) */
	if (((result >> 63) != (u_rt >> 63)) && ((u_rs >> 63) != (u_rt >> 63))) {
	  raise_overflow(s);
	  break;
	}
	s->gpr[rd] = (int64_t)result;
	s->pc += 4;
	s->insn_histo[mipsInsn::DSUB]++;
	break;
      }
      case 0x2F: /* dsubu */
	s->gpr[rd] = s->gpr[rs] - s->gpr[rt];
	s->pc += 4;
	s->insn_histo[mipsInsn::DSUBU]++;
	break;
      case 0x14: /* dsllv: rd = rt << rs[5:0] */
	s->gpr[rd] = s->gpr[rt] << (s->gpr[rs] & 63);
	s->pc += 4;
	s->insn_histo[mipsInsn::DSLLV]++;
	break;
      case 0x16: /* dsrlv: rd = rt >> rs[5:0] (logical) */
	s->gpr[rd] = (int64_t)((uint64_t)s->gpr[rt] >> (s->gpr[rs] & 63));
	s->pc += 4;
	s->insn_histo[mipsInsn::DSRLV]++;
	break;
      case 0x17: /* dsrav: rd = rt >> rs[5:0] (arithmetic) */
	s->gpr[rd] = s->gpr[rt] >> (s->gpr[rs] & 63);
	s->pc += 4;
	s->insn_histo[mipsInsn::DSRAV]++;
	break;
      case 0x38: /* dsll: rd = rt << sa */
	s->gpr[rd] = s->gpr[rt] << sa;
	s->pc += 4;
	s->insn_histo[mipsInsn::DSLL]++;
	break;
      case 0x3A: /* dsrl: rd = rt >> sa (logical) */
	s->gpr[rd] = (int64_t)((uint64_t)s->gpr[rt] >> sa);
	s->pc += 4;
	s->insn_histo[mipsInsn::DSRL]++;
	break;
      case 0x3B: /* dsra: rd = rt >> sa (arithmetic) */
	s->gpr[rd] = s->gpr[rt] >> sa;
	s->pc += 4;
	s->insn_histo[mipsInsn::DSRA]++;
	break;
      case 0x3C: /* dsll32: rd = rt << (sa + 32) */
	s->gpr[rd] = s->gpr[rt] << (sa + 32);
	s->pc += 4;
	s->insn_histo[mipsInsn::DSLL32]++;
	break;
      case 0x3E: /* dsrl32: rd = rt >> (sa + 32) (logical) */
	s->gpr[rd] = (int64_t)((uint64_t)s->gpr[rt] >> (sa + 32));
	s->pc += 4;
	s->insn_histo[mipsInsn::DSRL32]++;
	break;
      case 0x3F: /* dsra32: rd = rt >> (sa + 32) (arithmetic) */
	s->gpr[rd] = s->gpr[rt] >> (sa + 32);
	s->pc += 4;
	s->insn_histo[mipsInsn::DSRA32]++;
	break;
      default:
	raise_ri(s, inst);
	break;
      }
  }
  else if(isSpecial2)
    execSpecial2(inst,s);
  else if(isSpecial3)
    execSpecial3(inst,s);
  else if(isJType) {
    state_t::reg_t jaddr = inst & ((1<<26)-1);
    jaddr <<= 2;
    if(opcode==0x2) { /* j */
      s->pc += 4;
      s->insn_histo[mipsInsn::J]++;
    }
    else if(opcode==0x3) { /* jal */
      s->gpr[31] = sext32((uint32_t)(s->pc + 8));
      s->pc += 4;
      s->insn_histo[mipsInsn::JAL]++;
    }
    else {
      printf("Unknown JType instruction\n");
      exit(-1);
    }
    jaddr |= (s->pc & (~static_cast<state_t::reg_t>((1<<28)-1)));
    execMips<EL>(s);
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
	  s->insn_histo[mipsInsn::TLBR]++;
	  break;
	}
	case 0x2: { /* TLBWI -- write staging regs to TLB[Index] */
	  uint32_t idx = s->cpr0[CPR0_INDEX] & 63;
	  if(idx < (uint32_t)state_t::NUM_TLB_ENTRIES) {
	    s->tlb[idx].entry_hi  = s->cpr0_64[CPR0_ENTRYHI];
	    s->tlb[idx].entry_lo0 = s->cpr0_64[CPR0_ENTRYLO0];
	    s->tlb[idx].entry_lo1 = s->cpr0_64[CPR0_ENTRYLO1];
	    s->tlb[idx].page_mask = s->cpr0[CPR0_PAGEMASK];
	  }
	  s->insn_histo[mipsInsn::TLBWI]++;
	  break;
	}
	case 0x6: { /* TLBWR -- write staging regs to TLB[Random] */
	  uint32_t idx = s->cpr0[CPR0_RANDOM] & 63;
	  if(idx < (uint32_t)state_t::NUM_TLB_ENTRIES) {
	    s->tlb[idx].entry_hi  = s->cpr0_64[CPR0_ENTRYHI];
	    s->tlb[idx].entry_lo0 = s->cpr0_64[CPR0_ENTRYLO0];
	    s->tlb[idx].entry_lo1 = s->cpr0_64[CPR0_ENTRYLO1];
	    s->tlb[idx].page_mask = s->cpr0[CPR0_PAGEMASK];
	  }
	  /* Decrement Random, wrap to NUM_TLB_ENTRIES-1 when it reaches Wired */
	  {
	    uint32_t wired  = s->cpr0[CPR0_WIRED] & 63;
	    uint32_t random = s->cpr0[CPR0_RANDOM] & 63;
	    s->cpr0[CPR0_RANDOM] = (random <= wired)
	      ? (uint32_t)(state_t::NUM_TLB_ENTRIES - 1) : (random - 1);
	  }
	  s->insn_histo[mipsInsn::TLBWR]++;
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
	  s->insn_histo[mipsInsn::TLBP]++;
	  break;
	}
	case 24: { /* ERET -- exception return */
	  if(s->cpr0[CPR0_SR] & SR_ERL) {
	    /* Return from error: EPC = ErrorEPC, clear ERL */
	    s->pc = (int32_t)s->cpr0[CPR0_ERROREPC] - 4;
	    s->cpr0[CPR0_SR] &= ~SR_ERL;
	  } else {
	    /* Return from exception: PC = EPC, clear EXL */
	    s->pc = (int32_t)s->cpr0[CPR0_EPC] - 4;
	    s->cpr0[CPR0_SR] &= ~SR_EXL;
	  }
	  s->insn_histo[mipsInsn::ERET]++;
	  break;
	}
	case 32: //WAIT
	  if((s->cpr0[CPR0_SR] & 1) == 0) {
	    printf("attempting to wait with interrupts disabled @ VA %x, PA %x\n",
		   s->pc, VA2PA(s->pc));
	    exit(-1);
	  }
	  s->insn_histo[mipsInsn::WAIT]++;
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
      s->insn_histo[mipsInsn::DI]++;
    }
    else if( (((inst >> 21) & 31) == 11 ) &&
	     ((inst & 65535) == 0x6020) ) {
      //EI
      if(rt != 0) {
	s->gpr[rt] = s->cpr0[CPR0_SR];
      }
      s->cpr0[CPR0_SR] |= 1U;
      s->insn_histo[mipsInsn::EI]++;
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
	  s->insn_histo[mipsInsn::MFC0]++;
	  break;
	case 0x1: /*dmfc0 -- read full 64-bit CP0 register */
	  s->gpr[rt] = s->cpr0_64[rd];
	  s->insn_histo[mipsInsn::DMFC0]++;
	  break;
	case 0x4: /*mtc0*/
	  if(rd != 15) { /* PRId (reg 15) is read-only */
	    s->cpr0[rd] = (uint32_t)s->gpr[rt];
	    s->cpr0_64[rd] = (uint64_t)(uint32_t)s->gpr[rt];
	  }
	  /* CP0 reg 7 is the simulator putchar port */
	  if(rd == 7 && !s->silent) {
	    fputc((int)(s->gpr[rt] & 0xff), stdout);
	    fflush(stdout);
	  }
	  s->insn_histo[mipsInsn::MTC0]++;
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
	  s->insn_histo[mipsInsn::DMTC0]++;
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
	s->insn_histo[mipsInsn::ADDI]++;
	break;
      case 0x09: /* addiu */
	tmp = s->gpr[rs] + simm32;
	s->gpr[rt] = tmp;
	s->pc+=4;
	s->insn_histo[mipsInsn::ADDIU]++;
	break;
      case 0x0A: /* slti */
	s->gpr[rt] = (s->gpr[rs] < simm32);
	s->pc += 4;
	s->insn_histo[mipsInsn::SLTI]++;
	break;
      case 0x0B:/* sltiu */
	s->gpr[rt] = ((uint64_t)s->gpr[rs] < (uint64_t)(int64_t)simm32);
	s->pc += 4;
	s->insn_histo[mipsInsn::SLTIU]++;
	break;
      case 0x0c: /* andi */
	s->gpr[rt] = s->gpr[rs] & uimm32;
	s->pc += 4;
	s->insn_histo[mipsInsn::ANDI]++;
	break;
      case 0x0d: /* ori */
	s->gpr[rt] = s->gpr[rs] | uimm32;
	s->pc += 4;
	s->insn_histo[mipsInsn::ORI]++;
	break;
      case 0x0e: /* xori */
	s->gpr[rt] = s->gpr[rs] ^ uimm32;
	s->pc += 4;
	s->insn_histo[mipsInsn::XORI]++;
	break;
      case 0x0F: /* lui */
	uimm32 <<= 16;
	s->gpr[rt] = sext64(uimm32);
	s->pc += 4;
	s->insn_histo[mipsInsn::LUI]++;
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
      case 0x19: /* daddiu */
	s->gpr[rt] = s->gpr[rs] + simm32;
	s->pc += 4;
	s->insn_histo[mipsInsn::DADDIU]++;
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
	uint32_t ea  = va2pa(s->gpr[rs_] + imm);
	if(ea & 3) { raise_adel(s); break; }
	s->gpr[rt_] = (uint64_t)(uint32_t)bswap<EL>(s->mem.get<int32_t>(ea));
	s->pc += 4;
	s->insn_histo[mipsInsn::LWU]++;
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
      case 0x34: /* LLD: functional load-linked doubleword */
	_ld<EL>(inst, s);
	break;
      case 0x3D:
	_sdc1<EL>(inst, s);
	break;
      case 0x3F:
	_sd<EL>(inst, s);
	break;
      case 0x3C: /* SCD: store-conditional doubleword (always succeeds) */
	_scd<EL>(inst, s);
	break;
      default:
	raise_ri(s, inst);
	break;
      }
  }
}
