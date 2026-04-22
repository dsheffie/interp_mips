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
#include <map>
#include <stack>

#include "interpret.hh"
#include "disassemble.hh"
#include "helper.hh"
#include "globals.hh"

//#define CALLSTACK_DEBUG

#ifdef CALLSTACK_DEBUG
static std::stack<uint32_t> callstack;
#endif

enum class fpOperation {
  abs,neg,mov,add,
  sub,mul,div,sqrt,
  rsqrt,recip
};

enum class branch_type {
  beq, bne, blez, bgtz,
  beql, bnel, blezl, bgtzl,
  bgez, bgezl, bltz, bltzl,
  bc1f, bc1t, bc1fl, bc1tl
};


void execRType(uint32_t inst, state_t *s);
void execJType(uint32_t inst, state_t *s);
void execIType(uint32_t inst, state_t *s);
void execSpecial2(uint32_t inst, state_t *s);
void execSpecial3(uint32_t inst, state_t *s);
void execCoproc0(uint32_t inst, state_t *s);
void execCoproc2(uint32_t inst, state_t *s);

template <bool EL> void execMips(state_t *s);

template<bool iside=false>
uint32_t translate(state_t *s, uint32_t ea, int &fault) {
  fault = 0;
  uint32_t seg = (ea >> 28) & 0xf;
  //printf("ea %x maps to segment %d\n", ea, seg);
  switch(seg)
    {
    case 10: /* kseg1 */
    case 11: 
      return (ea & 0x1fffffff);
      break;
    default:
      fault = 1;
      break;
    }
  return ea;
}

template<typename T, bool EL>
T read_access(state_t *s, uint32_t pa) {
  uint8_t *mem = s->mem;
  if(pa >= 0x1fa00000 and pa <= 0x1fafffff) {
    uint32_t offs = pa & 0xfffff;
    return s->mc->read(offs);
  }
  
  T x = bswap<EL>(*(reinterpret_cast<T*>(mem + pa)));
  return x;
}

template<typename T, bool EL>
void store_access(T x, state_t *s, uint32_t pa) {
  uint8_t *mem = s->mem;
  if(pa >= 0x1fa00000 and pa <= 0x1fafffff) {
    uint32_t offs = pa & 0xfffff;
    s->mc->write(offs, x);
  }  
  *reinterpret_cast<T*>(s->mem + pa) = bswap<EL>(x);  
}

void execMipsEL(state_t *s) {
  //execMips<true>(s);
  assert(false);
}
void execMips(state_t *s) {
  execMips<false>(s);
}

std::ostream &operator<<(std::ostream &out, const state_t & s) {
  using namespace std;
  out << "PC : " << hex << s.last_pc << dec << "\n";
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
#endif
  for(int i = 0; i < 32; i++) {
    out << "cpr1_" << i << " : 0x"
	<< hex << s.cpr1[i] << dec
	<< "\n";
  }
#if 0
  for(int i = 0; i < 5; i++) {
    out << "fcr" << i << " : 0x"
	<< hex << s.fcr1[i] << dec
	<< "\n";
  }
#endif
  out << "icnt : " << s.icnt << "\n";
  return out;
}


static uint32_t getConditionCode(state_t *s, uint32_t cc);
static void setConditionCode(state_t *s, uint32_t v, uint32_t cc);


/* IType instructions */
static int _lb(uint32_t inst, state_t *s);
static int _lbu(uint32_t inst, state_t *s);
static int _sb(uint32_t inst, state_t *s);


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
  memset(s, 0, sizeof(state_t));
  /* setup the status register */
  s->cpr0[12] |= 1<<2;
  s->cpr0[12] |= 1<<22;
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




void execSpecial2(uint32_t inst,state_t *s)
{
  uint32_t funct = inst & 63; 
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;

  switch(funct)
    {
    case(0x0): /* madd */ {
      int64_t y,acc;
      acc = ((int64_t)s->hi) << 32;
      acc |= ((int64_t)s->lo);
      y = (int64_t)s->gpr[rs] * (int64_t)s->gpr[rt];
      y += acc;
      s->lo = (int32_t)(y & 0xffffffff);
      s->hi = (int32_t)(y >> 32);
      break;
    }
    case 0x1: /* maddu */ {
      uint64_t y,acc;
      uint32_t u0 = *((uint32_t*)&s->gpr[rs]);
      uint32_t u1 = *((uint32_t*)&s->gpr[rt]);
      uint64_t uk0 = (uint64_t)u0;
      uint64_t uk1 = (uint64_t)u1;
      y = uk0*uk1;
      acc = ((uint64_t)s->hi) << 32;
      acc |= ((uint64_t)s->lo);
      y += acc;
      s->lo = (uint32_t)(y & 0xffffffff);
      s->hi = (uint32_t)(y >> 32);
      break;
    }
    case(0x2): /* mul */{
      int64_t y = ((int64_t)s->gpr[rs]) * ((int64_t)s->gpr[rt]);
      s->gpr[rd] = (int32_t)y;
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
      break;
    }

    case(0x20): /* clz */
      s->gpr[rd] = (s->gpr[rs]==0) ? 32 : __builtin_clz(s->gpr[rs]);
      break;
    default:
      printf("unhandled special2 instruction @ 0x%08x\n", s->pc); 
      exit(-1);
      break;
    }
  s->pc += 4;
}

void execSpecial3(uint32_t inst,state_t *s) {
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
	break;
      case 0x18: /* seh */
	s->gpr[rd] = (int32_t)((int16_t)s->gpr[rt]);
	break;
      default:
	printf("unhandled special3 instruction @ 0x%08x\n", s->pc); 
	exit(-1);    
	break;
      }
  }
  else if(funct == 0) { /* ext */  
    uint32_t pos = (inst >> 6) & 31;
    uint32_t size = ((inst >> 11) & 31) + 1;
    s->gpr[rt] = (s->gpr[rs] >> pos) & ((1<<size)-1);
  }
  else if(funct == 0x4) {/* ins */
    uint32_t size = rd-op+1;
    uint32_t mask = (1U<<size) -1;
    uint32_t cmask = ~(mask << op);
    uint32_t v = (s->gpr[rs] & mask) << op;
    uint32_t c = (s->gpr[rt] & cmask) | v;
    s->gpr[rt] = c;
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
int lxc1(uint32_t inst, state_t *s) {
  mips_t mi(inst);
  int fault;
  uint32_t ea = s->gpr[mi.lc1x.base] + s->gpr[mi.lc1x.index];
  uint32_t pa = translate(s, ea, fault);
  if(fault) {
    return fault;
  }
  *reinterpret_cast<T*>(s->cpr1 + mi.lc1x.fd) = read_access<T, EL>(s, pa);
  s->pc += 4;
  return 0;
}

template <bool EL>
int execCoproc1x(uint32_t inst, state_t *s) {
  mips_t mi(inst);
  int fault = 0;
  switch(mi.lc1x.id)
    {
    case 0:
      //lwxc1
      return lxc1<EL,int32_t>(inst, s);
    case 1:
      //ldxc1
      return lxc1<EL,int64_t>(inst, s);
    default:
      break;
    }

  switch(mi.c1x.fmt)
   {
   case 0: {
     c1xExec<float> e;
     e(mi.c1x, s);
     return 0;
   }
   case 1: {
     c1xExec<double> e;
     e(mi.c1x, s);
     return 0;
   }
   default:
     std::cerr << "weird type in do_c1x_op @ 0x"
	       << std::hex << s->pc << std::dec
	       <<"\n";
     exit(-1);
   }

  return 0;
}



template <bool EL, branch_type bt>
void branch(uint32_t inst, state_t *s) {
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
  uint32_t npc = s->pc+4; 
  bool isLikely = false, takeBranch = false;
  switch(bt)
    {
    case branch_type::beql:
      isLikely = true;
    case branch_type::beq:
      takeBranch = (s->gpr[rt] == s->gpr[rs]);
      break;
    case branch_type::bnel:
      isLikely = true;
    case branch_type::bne:
      takeBranch = (s->gpr[rt] != s->gpr[rs]);
      break;
    case branch_type::blezl:
      isLikely = true;
    case branch_type::blez:
      takeBranch = (s->gpr[rs] <= 0);
      break;
    case branch_type::bgtzl:
      isLikely = true;
    case branch_type::bgtz:
      takeBranch = (s->gpr[rs] > 0);
      break;
    case branch_type::bgezl:
      isLikely = true;
    case branch_type::bgez:
      takeBranch = (s->gpr[rs] >= 0);
      break;
    case branch_type::bltzl:
      isLikely = true;
    case branch_type::bltz:
      takeBranch = (s->gpr[rs] < 0);
      break;
    case branch_type::bc1tl:
      isLikely = true;
    case branch_type::bc1t:
      takeBranch = getConditionCode(s,((inst>>18)&7))==1;
      break;
    case branch_type::bc1fl:
      isLikely = true;
    case branch_type::bc1f:
      takeBranch = getConditionCode(s,((inst>>18)&7))==0;
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
      s->pc = (imm+npc);
    }
  }
}

template <bool EL>
void _bgez_bltz(uint32_t inst, state_t *s) {
  uint32_t rt = (inst >> 16) & 31;
  switch(rt&3)
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
    }
}


template <bool EL>
int _lw(uint32_t inst, state_t *s) {
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  uint32_t ea = (uint32_t)s->gpr[rs] + imm;
  int fault;  
  uint32_t pa = translate(s, ea, fault);
  if(fault) {
    return fault;
  }
  int32_t x = read_access<int32_t, EL>(s, pa);
  s->gpr[rt] = x;
  s->pc += 4;
  return 0;
}

template <bool EL>
int _lh(uint32_t inst, state_t *s) {
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  uint32_t ea = s->gpr[rs] + imm;
  int fault;  
  uint32_t pa = translate(s, ea, fault);
  if(fault) {
    return fault;
  }
  int16_t x = read_access<int16_t, EL>(s, pa);
  s->gpr[rt] = (int32_t)x;
  s->pc +=4;
  return 0;
}

template <bool EL>
int _lb(uint32_t inst, state_t *s){
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  int fault;
  uint32_t ea = s->gpr[rs] + imm;
  uint32_t pa = translate(s, ea, fault);
  if(fault) {
    return fault;
  }
  int8_t x = read_access<int8_t,EL>(s, pa);  
  s->gpr[rt] = (int32_t)x;
  s->pc += 4;
  return 0;
}

static int
_lbu(uint32_t inst, state_t *s) {
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  int fault;
  uint32_t ea = s->gpr[rs] + imm;
  uint32_t pa = translate(s, ea, fault);
  if(fault) {
    return fault;
  }  
  uint32_t zExt = (uint32_t)s->mem[pa];
  *((uint32_t*)&(s->gpr[rt])) = zExt;
  s->pc += 4;
  return fault;
}

template <bool EL>
int _lhu(uint32_t inst, state_t *s) {
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  int fault;
  uint32_t ea = s->gpr[rs] + imm;
  uint32_t pa = translate(s, ea, fault);
  if(fault) {
    return fault;
  }
  uint16_t x = read_access<uint16_t, EL>(s, pa);  
  *((uint32_t*)&(s->gpr[rt])) = static_cast<uint32_t>(x);
  s->pc += 4;
  return 0;
}

template <bool EL>
int _sw(uint32_t inst, state_t *s) {
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  int fault;
  uint32_t ea = s->gpr[rs] + imm;
  uint32_t pa = translate(s, ea, fault);
  if(fault) {
    return fault;
  }
  store_access<int32_t, EL>(s->gpr[rt], s, pa);
  s->pc += 4;
  return 0;
}

template <bool EL>
void _sc(uint32_t inst, state_t *s) {
  uint32_t rt = (inst >> 16) & 31;
  _sw<EL>(inst, s);
  s->gpr[rt] = 1;
}


template <bool EL>
int _sh(uint32_t inst, state_t *s) {
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
    
  uint32_t ea = s->gpr[rs] + imm;
  int fault;
  uint32_t pa = translate(s, ea, fault);
  if(fault) {
    return fault;
  }
  store_access<int16_t, EL>(static_cast<int16_t>(s->gpr[rt]), s, pa);  
  s->pc += 4;
  return 0;
}

template <bool EL>
int _sb(uint32_t inst, state_t *s) {
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  int fault;
  uint32_t ea = s->gpr[rs] + imm;
  uint32_t pa = translate(s, ea, fault);
  if(fault) {
    return fault;
  }
  store_access<uint8_t, EL>(static_cast<uint8_t>(s->gpr[rt]), s, pa);    
  s->pc +=4;
  return 0;
}

static void _mtc1(uint32_t inst, state_t *s) {
  uint32_t rd = (inst>>11) & 31;
  uint32_t rt = (inst>>16) & 31;
  s->cpr1[rd] = s->gpr[rt];
  s->pc += 4;
}

static void _mfc1(uint32_t inst, state_t *s) {
  uint32_t rd = (inst>>11) & 31;
  uint32_t rt = (inst>>16) & 31;
  s->gpr[rt] = s->cpr1[rd];
  s->pc +=4;
}


template <bool EL>
int _swl(uint32_t inst, state_t *s) {
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  int fault;
  uint32_t ea = s->gpr[rs] + imm;
  uint32_t ma = ea & 3;
  ea &= 0xfffffffc;
  uint32_t pa = translate(s, ea, fault);
  if(fault) {
    return fault;
  }
  if(EL)
    ma = 3 - ma;
  uint32_t r = read_access<int32_t, EL>(s, pa); 
  uint32_t xx=0,x = s->gpr[rt];
  
  uint32_t xs = x >> (8*ma);
  uint32_t m = ~((1U << (8*(4 - ma))) - 1);
  xx = (r & m) | xs;
  store_access<uint32_t, EL>(xx, s, pa);
  s->pc += 4;
  return 0;
}

template <bool EL>
int _swr(uint32_t inst, state_t *s) {
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  int fault;
  uint32_t ea = s->gpr[rs] + imm;
  uint32_t ma = ea & 3;
  if(EL)
    ma = 3 - ma;
  ea &= 0xfffffffc;
  uint32_t pa = translate(s, ea, fault);
  if(fault) {
    return fault;
  }
  uint32_t r = read_access<int32_t, EL>(s, pa);   
  uint32_t xx=0,x = s->gpr[rt];
  
  uint32_t xs = 8*(3-ma);
  uint32_t rm = (1U << xs) - 1;

  xx = (x << xs) | (rm & r);
  store_access<uint32_t, EL>(xx, s, pa);  
  s->pc += 4;
  return 0;
}

template <bool EL>
int _lwl(uint32_t inst, state_t *s) {
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  int fault;
  uint32_t ea = ((uint32_t)s->gpr[rs] + imm);
  uint32_t ma = ea & 3;
  ea &= 0xfffffffc;
  uint32_t pa = translate(s, ea, fault);
  if(fault) {
    return fault;
  }
  if(EL)
    ma = 3 - ma;
  int32_t r = read_access<int32_t, EL>(s, pa);
  int32_t x =  s->gpr[rt];
  
  switch(ma)
    {
    case 0:
      s->gpr[rt] = r;
      break;
    case 1:
      s->gpr[rt] = ((r & 0x00ffffff) << 8) | (x & 0x000000ff) ;
      break;
    case 2:
      s->gpr[rt] = ((r & 0x0000ffff) << 16)  | (x & 0x0000ffff) ;
      break;
    case 3:
      s->gpr[rt] = ((r & 0x00ffffff) << 24)  | (x & 0x00ffffff);
      break;
    }
  s->pc += 4;
  return 0;
}

template<bool EL>
int _lwr(uint32_t inst, state_t *s) {
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  int fault;
  uint32_t ea = ((uint32_t)s->gpr[rs] + imm);
  uint32_t ma = ea & 3;
  ea &= 0xfffffffc;
  if(EL)
    ma = 3-ma;
  uint32_t pa = translate(s, ea, fault);
  if(fault) {
    return fault;
  }
  int32_t r = read_access<int32_t, EL>(s, pa);
  uint32_t x =  s->gpr[rt];

  switch(ma)
    {
    case 0:
      s->gpr[rt] = (x & 0xffffff00) | (r>>24);
      break;
    case 1:
      s->gpr[rt] = (x & 0xffff0000) | (r>>16);
      break;
    case 2:
      s->gpr[rt] = (x & 0xff000000) | (r>>8);
      break;
    case 3:
      s->gpr[rt] = r;
      break;
    }
  s->pc += 4;
  return 0;
}



template <bool EL>
int _ldc1(uint32_t inst, state_t *s) {
  uint32_t ft = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  int fault;
  uint32_t ea = s->gpr[rs] + imm;
  uint32_t pa = translate(s, ea, fault);
  if(fault) {
    return fault;
  }
  *((int64_t*)(s->cpr1 + ft)) = read_access<int64_t, EL>(s, pa);
  s->pc += 4;
  return 0;
}

template <bool EL>
int _sdc1(uint32_t inst, state_t *s) {
  uint32_t ft = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  uint32_t ea = s->gpr[rs] + imm;
  int fault;
  uint32_t pa = translate(s, ea, fault);
  if(fault) {
    return fault;
  }
  store_access<int64_t, EL>((*(int64_t*)(s->cpr1 + ft)), s, pa);
  s->pc += 4;
  return 0;
}

template <bool EL>
int _lwc1(uint32_t inst, state_t *s) {
  uint32_t ft = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  uint32_t ea = s->gpr[rs] + imm;
  int fault;
  uint32_t pa = translate(s, ea, fault);
  if(fault) {
    return fault;
  }
  *((uint32_t*)(s->cpr1 + ft)) = read_access<uint32_t, EL>(s, pa);  
  s->pc += 4;
  return 0;
}

template <bool EL>
int _swc1(uint32_t inst, state_t *s) {
  uint32_t ft = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  uint32_t ea = s->gpr[rs] + imm;
  int fault;
  uint32_t pa = translate(s, ea, fault);
  if(fault) {
    return fault;
  }
  store_access<uint32_t, EL>((*(uint32_t*)(s->cpr1 + ft)), s, pa);  
  s->pc += 4;
  return 0;
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
  switch(fmt)
    {
    case FMT_S:
      f = (*((float*)(s->cpr1 + fs)));
      //printf("f=%g\n", f);
      *ptr = (int32_t)f;
      break;
    case FMT_D:
      d = (*((double*)(s->cpr1 + fs)));
      //printf("d=%g\n", d);
      *ptr = (int32_t)d;
      //printf("id=%d\n", *ptr);
      break;
    default:
      printf("unknown trunc for fmt %d\n", fmt);
      exit(-1);
      break;
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
  }
  else {
    if(getConditionCode(s,cc)==1) {
      s->cpr1[fd+0] = s->cpr1[fs+0];
      s->cpr1[fd+1] = s->cpr1[fs+1];
    }
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
  }
  else {
    s->cpr1[fd+0] = getConditionCode(s, cc) ? s->cpr1[fs+0] : s->cpr1[fd+0];
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
  }
  else {
    /* movt */
    s->gpr[rd] = getConditionCode(s, cc) ? s->gpr[rs] : s->gpr[rd];
  }
  s->pc += 4;
}

static void _cvts(uint32_t inst, state_t *s) {
  uint32_t fmt = (inst >> 21) & 31;
  uint32_t fd = (inst>>6) & 31;
  uint32_t fs = (inst>>11) & 31;
  switch(fmt)
    {
    case FMT_D:
      *((float*)(s->cpr1 + fd)) = (float)(*((double*)(s->cpr1 + fs)));
      break;
    case FMT_W:
      *((float*)(s->cpr1 + fd)) = (float)(*((int32_t*)(s->cpr1 + fs)));
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
      s->fcr1[CP1_CR25] = setBit(s->fcr1[CP1_CR25],v,cc);
      break;
    case COND_LT:
      v = (Tfs < Tft);
      s->fcr1[CP1_CR25] = setBit(s->fcr1[CP1_CR25],v,cc);
      break;
    case COND_LE:
      v = (Tfs <= Tft);
      s->fcr1[CP1_CR25] = setBit(s->fcr1[CP1_CR25],v,cc);
      break;
    default:
      printf("unimplemented %s = %s\n", __func__, getCondName(cond).c_str());
      exit(-1);
      break;
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
void execFP(uint32_t inst, state_t *s) {
  uint32_t ft = (inst>>16)&31, fs=(inst>>11)&31, fd=(inst>>6)&31;
  T _fs = *reinterpret_cast<T*>(s->cpr1+fs);
  T _ft = *reinterpret_cast<T*>(s->cpr1+ft);
  T &_fd = *reinterpret_cast<T*>(s->cpr1+fd);

  switch(op)
    {
    case fpOperation::abs:
      _fd = std::abs(_fs);
      break;
    case fpOperation::neg:
      _fd = -_fs;
      break;
    case fpOperation::mov:
      _fd = _fs;
      break;
    case fpOperation::add:
      _fd = _fs + _ft;
      break;
    case fpOperation::sub:
      _fd = _fs - _ft;
      break;
    case fpOperation::mul:
      _fd = _fs * _ft;
      break;
    case fpOperation::div:
      if(_ft==0.0) {
	_fd = std::numeric_limits<T>::max();
      }
      else {
	_fd = _fs / _ft;
      }
      break;
    case fpOperation::sqrt:
      _fd = std::sqrt(_fs);
      break;
    case fpOperation::rsqrt:
      _fd = static_cast<T>(1.0) / std::sqrt(_fs);
      break;
    case fpOperation::recip:
      _fd = static_cast<T>(1.0) / _fs;
      break;
    default:
      UNREACHABLE();
    }
  s->pc+=4;
}

template <fpOperation op>
void do_fp_op(uint32_t inst, state_t *s) {
  switch((inst>>21)&31) {
  case FMT_S:
    execFP<float,op>(inst,s);
    break;
  case FMT_D:
    execFP<double,op>(inst,s);
    break;
  default:
    UNREACHABLE();
  }
}


template <bool EL>
void execCoproc1(uint32_t inst, state_t *s) {
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

static void _sll(uint32_t inst, state_t *s) {
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  uint32_t sa = (inst >> 6) & 31;
  s->gpr[rd] = s->gpr[rt] << sa;
  if(inst == 0) {
    s->nopcnt++;
  }
  s->pc += 4;
}

static void _srl(uint32_t inst, state_t *s) {
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  uint32_t sa = (inst >> 6) & 31;
  s->gpr[rd] = ((uint32_t)s->gpr[rt] >> sa);
  s->pc += 4;
}


static void _sra(uint32_t inst, state_t *s) {
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  uint32_t sa = (inst >> 6) & 31;
  s->gpr[rd] = s->gpr[rt] >> sa;
  s->pc += 4;
}


static void _sllv(uint32_t inst, state_t *s) {
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  s->gpr[rd] = s->gpr[rt] << (s->gpr[rs] & 0x1f);
  s->pc += 4;
}


static void _srlv(uint32_t inst, state_t *s) {
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  s->gpr[rd] = ((uint32_t)s->gpr[rt]) >> (s->gpr[rs] & 0x1f);  
  s->pc += 4;
}


static void _srav(uint32_t inst, state_t *s) {
  uint32_t rs = (inst >> 21) & 31;  
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  s->gpr[rd] = s->gpr[rt] >> (s->gpr[rs] & 0x1f);  
  s->pc += 4;
}

static void _jr(uint32_t inst, state_t *s) {
  uint32_t rs = (inst >> 21) & 31;  
  uint32_t jaddr = s->gpr[rs];
  s->pc += 4;
  execMips<false>(s);
  s->pc = jaddr;
}

static void _jalr(uint32_t inst, state_t *s) {
  uint32_t rs = (inst >> 21) & 31;  
  uint32_t jaddr = s->gpr[rs];
  s->gpr[31] = s->pc+8;
  s->pc += 4;
  execMips<false>(s);
  s->pc = jaddr;
}

static void _break(uint32_t inst, state_t *s) {
  s->brk = 1;
  s->pc += 4;
}

static void _sync(uint32_t inst, state_t *s) {
  s->pc += 4;
}

static void _mfhi(uint32_t inst, state_t *s) {
  uint32_t rd = (inst >> 11) & 31;
  s->gpr[rd] = s->hi;
  s->pc += 4;
}

static void _mthi(uint32_t inst, state_t *s) {
  uint32_t rs = (inst >> 21) & 31;  
  s->hi = s->gpr[rs];
  s->pc += 4;
}

static void _mflo(uint32_t inst, state_t *s) {
  uint32_t rd = (inst >> 11) & 31;
  s->gpr[rd] = s->lo;
  s->pc += 4;
}

static void _mtlo(uint32_t inst, state_t *s) {
  uint32_t rs = (inst >> 21) & 31;  
  s->lo = s->gpr[rs];
  s->pc += 4;
}

static void _mult(uint32_t inst, state_t *s) {
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  int64_t y  = (int64_t)s->gpr[rs] * (int64_t)s->gpr[rt];
  s->lo = (int32_t)(y & 0xffffffff);
  s->hi = (int32_t)(y >> 32);
  s->pc += 4;
}

static void _multu(uint32_t inst, state_t *s) {
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;

  uint64_t y;
  uint64_t u0 = (uint64_t)*((uint32_t*)&s->gpr[rs]);
  uint64_t u1 = (uint64_t)*((uint32_t*)&s->gpr[rt]);
  y = u0*u1;
  *((uint32_t*)&(s->lo)) = (uint32_t)y;
  *((uint32_t*)&(s->hi)) = (uint32_t)(y>>32);
  s->pc += 4;
}

static void _div(uint32_t inst, state_t *s) {
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  if(s->gpr[rt] != 0) {
    s->lo = (uint32_t)s->gpr[rs] / (uint32_t)s->gpr[rt];
    s->hi = (uint32_t)s->gpr[rs] % (uint32_t)s->gpr[rt];
  }
  s->pc += 4;
}

static void _divu(uint32_t inst, state_t *s) {
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  if(s->gpr[rt] != 0) {
    s->lo = (uint32_t)s->gpr[rs] / (uint32_t)s->gpr[rt];
    s->hi = (uint32_t)s->gpr[rs] % (uint32_t)s->gpr[rt];
  }
  s->pc += 4;
}

static void _add(uint32_t inst, state_t *s) {
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  s->gpr[rd] = s->gpr[rs] + s->gpr[rt];
  s->pc += 4;
}

static void _addu(uint32_t inst, state_t *s) {
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  uint32_t u_rs = (uint32_t)s->gpr[rs];
  uint32_t u_rt = (uint32_t)s->gpr[rt];
  s->gpr[rd] = u_rs + u_rt;
  s->pc += 4;
}

static void _subu(uint32_t inst, state_t *s) {
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  uint32_t u_rs = (uint32_t)s->gpr[rs];
  uint32_t u_rt = (uint32_t)s->gpr[rt];
  uint32_t y = u_rs - u_rt;
  s->gpr[rd] = y;
  s->pc += 4;
}

static void _and(uint32_t inst, state_t *s) {
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  s->gpr[rd] = s->gpr[rs] & s->gpr[rt];
  s->pc += 4;
}

static void _or(uint32_t inst, state_t *s) {
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  s->gpr[rd] = s->gpr[rs] | s->gpr[rt];
  s->pc += 4;
}

static void _xor(uint32_t inst, state_t *s) {
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  s->gpr[rd] = s->gpr[rs] ^ s->gpr[rt];
  s->pc += 4;
}

static void _nor(uint32_t inst, state_t *s) {
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  s->gpr[rd] = ~(s->gpr[rs] | s->gpr[rt]);
  s->pc += 4;
}

static void _slt(uint32_t inst, state_t *s) {
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  s->gpr[rd] = s->gpr[rs] < s->gpr[rt];
  s->pc += 4;
}

static void _sltu(uint32_t inst, state_t *s) {
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  uint32_t urs = (uint32_t)s->gpr[rs];
  uint32_t urt = (uint32_t)s->gpr[rt];
  s->gpr[rd] = (urs < urt);
  s->pc += 4;
}

static void _movn(uint32_t inst, state_t *s) {
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  s->gpr[rd] = (s->gpr[rt] != 0) ? s->gpr[rs] : s->gpr[rd];
  s->pc +=4;
}

static void _movz(uint32_t inst, state_t *s) {
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  s->gpr[rd] = (s->gpr[rt] == 0) ? s->gpr[rs] : s->gpr[rd];
  s->pc +=4;
}

static void _teq(uint32_t inst, state_t *s) {
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  if(s->gpr[rs] == s->gpr[rt]) {
    printf("teq trap!!!!!\n");
    exit(-1);
  }
  s->pc +=4;
}
/* end rtype */

static void _j(uint32_t inst, state_t *s) {
  uint32_t jaddr = inst & ((1<<26)-1);
  jaddr <<= 2;  
  s->pc += 4;
  jaddr |= (s->pc & (~((1<<28)-1)));
  execMips<false>(s);
  s->pc = jaddr;  
}

static void _jal(uint32_t inst, state_t *s) {
  uint32_t jaddr = inst & ((1<<26)-1);
  jaddr <<= 2;  
  s->gpr[31] = s->pc+8;  
  s->pc += 4;
  jaddr |= (s->pc & (~((1<<28)-1)));
  execMips<false>(s);
  s->pc = jaddr;  
}

/* end jtype */

static void _addi(uint32_t inst, state_t *s) {
  uint32_t uimm32 = inst & ((1<<16) - 1);
  int16_t simm16 = (int16_t)uimm32;
  int32_t simm32 = (int32_t)simm16;
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  s->gpr[rt] = s->gpr[rs] + simm32;
  s->pc+=4;  
}

static void _addiu(uint32_t inst, state_t *s) {
  uint32_t uimm32 = inst & ((1<<16) - 1);
  int16_t simm16 = (int16_t)uimm32;
  int32_t simm32 = (int32_t)simm16;
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  s->gpr[rt] = s->gpr[rs] + simm32;
  s->pc+=4;  
}

static void _slti(uint32_t inst, state_t *s) {
  uint32_t uimm32 = inst & ((1<<16) - 1);
  int16_t simm16 = (int16_t)uimm32;
  int32_t simm32 = (int32_t)simm16;
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  s->gpr[rt] = (s->gpr[rs] < simm32);
  s->pc+=4;  
}

static void _sltiu(uint32_t inst, state_t *s) {
  uint32_t uimm32 = inst & ((1<<16) - 1);
  int16_t simm16 = (int16_t)uimm32;
  int32_t simm32 = (int32_t)simm16;
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  s->gpr[rt] = ((uint32_t)s->gpr[rs] < (uint32_t)simm32);
  s->pc+=4;  
}

static void _andi(uint32_t inst, state_t *s) {
  uint32_t uimm32 = inst & ((1<<16) - 1);
  int16_t simm16 = (int16_t)uimm32;
  int32_t simm32 = (int32_t)simm16;
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  s->gpr[rt] = s->gpr[rs] & uimm32;
  s->pc+=4;  
}

static void _ori(uint32_t inst, state_t *s) {
  uint32_t uimm32 = inst & ((1<<16) - 1);
  int16_t simm16 = (int16_t)uimm32;
  int32_t simm32 = (int32_t)simm16;
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  s->gpr[rt] = s->gpr[rs] | uimm32;
  s->pc+=4;  
}

static void _xori(uint32_t inst, state_t *s) {
  uint32_t uimm32 = inst & ((1<<16) - 1);
  int16_t simm16 = (int16_t)uimm32;
  int32_t simm32 = (int32_t)simm16;
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  s->gpr[rt] = s->gpr[rs] ^ uimm32;
  s->pc+=4;  
}

static void _lui(uint32_t inst, state_t *s) {
  uint32_t rt = (inst >> 16) & 31;
  uint32_t uimm32 = inst & ((1<<16) - 1);
  int16_t simm16 = (int16_t)uimm32;
  int32_t simm32 = (int32_t)simm16;
  uimm32 <<= 16;
  s->gpr[rt] = uimm32;
  s->pc+=4;  
}

static void _bgez_bltz_be(uint32_t inst, state_t *s) {
  _bgez_bltz<false>(inst, s); 
}

static void _beq_be(uint32_t inst, state_t *s) {
  branch<false,branch_type::beq>(inst, s); 
}

static void _bne_be(uint32_t inst, state_t *s) {
  branch<false,branch_type::bne>(inst, s); 
}

static void _blez_be(uint32_t inst, state_t *s) {
  branch<false,branch_type::blez>(inst, s); 
}

static void _bgtz_be(uint32_t inst, state_t *s) {
  branch<false,branch_type::bgtz>(inst, s); 
}

static void _beql_be(uint32_t inst, state_t *s) {
  branch<false,branch_type::beql>(inst, s); 
}

static void _blezl_be(uint32_t inst, state_t *s) {
  branch<false,branch_type::blezl>(inst, s); 
}

static void _bnel_be(uint32_t inst, state_t *s) {
  branch<false,branch_type::bnel>(inst, s); 
}

static void _bgtzl_be(uint32_t inst, state_t *s) {
  branch<false,branch_type::bgtzl>(inst, s); 
}

typedef void (*func_t)(uint32_t,state_t*);

static const func_t jtype_funcs[4] = {
  nullptr,
  nullptr,
  _j,
  _jal
};

static const func_t rtype_functs[64] = {
  _sll, /* 0 */
  _movci, /* 1 */
  _srl, /* 2 */
  _sra, /* 3 */
  _sllv, /* 4 */
  nullptr, /* 5 */
  _srlv, /* 6 */
  _srav, /* 7 */
  _jr, /* 8 */
  _jalr, /* 9 */
  _movz, /* a */
  _movn, /* b */
  nullptr, /* c - syscall */
  _break, /* d */
  nullptr, /* e */
  _sync, /* f */
  _mfhi, /* 10 */
  _mthi, /* 11 */
  _mflo, /* 12 */
  _mtlo, /* 13 */
  nullptr, /* 14 */
  nullptr, /* 15 */
  nullptr, /* 16 */
  nullptr, /* 17 */
  _mult, /* 18 */
  _multu, /* 19 */
  _div, /* 1a */
  _divu, /* 1b */
  nullptr, /* 1c */
  nullptr, /* 1d */
  nullptr, /* 1e */
  nullptr, /* 1f */
  _add, /* 20 */
  _addu, /* 21 */
  nullptr, /* 22 - sub */
  _subu, /* 23 */
  _and, /* 24 */
  _or, /* 25 */
  _xor, /* 26 */
  _nor, /* 27 */
  nullptr, /* 28 */
  nullptr, /* 29 */
  _slt, /* 2a */
  _sltu, /* 2b */
  nullptr, /* 2c */
  nullptr, /* 2d */
  nullptr, /* 2e */
  nullptr, /* 2f */
  nullptr, /* 30 */
  nullptr, /* 31 */
  nullptr, /* 32 */
  nullptr, /* 33 */
  _teq, /* 34 */ 
  nullptr, /* 35 */
  nullptr, /* 36 */
  nullptr, /* 37 */
  nullptr, /* 38 */
  nullptr, /* 39 */
  nullptr, /* 3a */
  nullptr, /* 3b */
  nullptr, /* 3c */
  nullptr, /* 3d */
  nullptr, /* 3e */
  nullptr  /* 3f */  
};

static const func_t itype_functs[64] = {
  nullptr, /* 0 */
  _bgez_bltz_be, /* 1 */
  nullptr, /* 2 */
  nullptr, /* 3 */
  _beq_be, /* 4 */
  _bne_be, /* 5 */
  _blez_be, /* 6 */
  _bgtz_be, /* 7 */
  _addi, /* 8 */
  _addiu, /* 9 */
  _slti, /* a */
  _sltiu, /* b */
  _andi, /* c */
  _ori, /* d */
  _xori, /* e */
  _lui,  /* f */
  nullptr, /* 10 */
  nullptr, /* 11 */
  nullptr, /* 12 */
  nullptr, /* 13 */
  _beql_be, /* 14 */
  _bnel_be, /* 15 */
  _blezl_be, /* 16 */
  _bgtzl_be, /* 17 */
  nullptr, /* 18 */
  nullptr, /* 19 */
  nullptr, /* 1a */
  nullptr, /* 1b */
  nullptr, /* 1c */
  nullptr, /* 1d */
  nullptr, /* 1e */
  nullptr, /* 1f */
  nullptr, /*_lb,*/ /* 20 */
  nullptr, /*_lh_be,*/ /* 21 */
  nullptr, /* 22 - sub */
  nullptr,/*_lw_be,*/ /* 23 */
  nullptr,/*_lbu*,/ /* 24 */
  nullptr,/*_lhu_be,*/ /* 25 */
  nullptr, /* 26 */
  nullptr, /* 27 */
  nullptr, /*_sb,*/ /* 28 */
  nullptr, /*_sh_be,*/ /* 29 */
  nullptr, /* 2a */
  nullptr, /*_sw_be,*/ /* 2b */
  nullptr, /* 2c */
  nullptr, /* 2d */
  nullptr, /* 2e */
  nullptr, /* 2f */
  nullptr, /* 30 */
  nullptr, /*_lwc1_be,*/ /* 31 */
  nullptr, /* 32 */
  nullptr, /* 33 */
  nullptr, /* 34 */ 
  nullptr, /*_ldc1_be,*/ /* 35 */
  nullptr, /* 36 */
  nullptr, /* 37 */
  nullptr, /* 38 */
  nullptr, /* 39 */
  nullptr, /* 3a */
  nullptr, /* 3b */
  nullptr, /* 3c */
  nullptr, /* 3d */
  nullptr, /* 3e */
  nullptr  /* 3f */  
};



template <bool EL>
void execMips(state_t *s) {
  uint8_t *mem = s->mem;
  int fault = 0;
  uint32_t opcode,rs,rt,rd,inst; 
  bool isRType,isJType,isCoproc0,isCoproc1,isCoproc1x,isCoproc2;
  bool isSpecial2,isSpecial3,isLoadLinked,isStoreCond;
  uint32_t ppc = translate<true>(s, s->pc, fault);
  if(fault) {
    printf("generating fetch fault\n");
    goto handle_exception;
  }
  inst = bswap<EL>(*(uint32_t*)(mem + ppc));
  //globals::execHisto[s->pc]++;
  s->last_pc = s->pc;  
  static_assert(EL==false, "not build for big endian");
  if(globals::log) {
    std::cout << std::hex << s->pc << std::dec << " : "
	      << getAsmString(inst, s->pc) << "\n";


  }

  opcode = inst>>26;
  isRType = (opcode==0);
  isJType = ((opcode>>1)==1);
  isCoproc0 = (opcode == 0x10);
  isCoproc1 = (opcode == 0x11);
  isCoproc1x = (opcode == 0x13);
  isCoproc2 = (opcode == 0x12);
  isSpecial2 = (opcode == 0x1c); 
  isSpecial3 = (opcode == 0x1f);
  isLoadLinked = (opcode == 0x30);
  isStoreCond = (opcode == 0x38);
  rs = (inst >> 21) & 31;
  rt = (inst >> 16) & 31;
  rd = (inst >> 11) & 31;
  //std::cout << getGPRName(R_a1) << " " << s->gpr[R_a1] << "\n";  
  s->icnt++;
    
  if(isRType) {
    uint32_t funct = inst & 63;
    uint32_t sa = (inst >> 6) & 31;
    auto f = rtype_functs[funct];
    assert(f != nullptr);
    f(inst, s);
  }
  else if(isSpecial2)
    execSpecial2(inst,s);
  else if(isSpecial3)
    execSpecial3(inst,s);
  else if(isJType) {
    auto f = jtype_funcs[opcode & 3];
    assert(f);
    f(inst, s);
  }
  else if(isCoproc0) {  
    switch(rs) 
      {
      case 0x0: /*mfc0*/
	s->gpr[rt] = s->cpr0[rd];
	break;
      case 0x4: /*mtc0*/
	s->cpr0[rd] = s->gpr[rt];
	break;
      default:
	printf("unknown %s instruction @ %x", __func__, s->pc); exit(-1);
	break;
      }
    s->pc += 4;
  }
  else if(isCoproc1) 
    execCoproc1<EL>(inst,s);
  else if(isCoproc1x) {
    fault = execCoproc1x<EL>(inst,s);
  }
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
    auto f = itype_functs[opcode];
    if(f) {
      f(inst, s);
      return;
    }
    switch(opcode) 
      {
      case 0x20:
	fault = _lb<EL>(inst, s);
	break;
      case 0x21:
	fault = _lh<EL>(inst, s);
	break;	
      case 0x22: 
	fault = _lwl<EL>(inst, s);
	break;
      case 0x23:
	fault = _lw<EL>(inst, s);
	break;
      case 0x24:
	fault = _lbu(inst, s);
	break;
      case 0x25:
	fault = _lhu<EL>(inst, s);
	break;
      case 0x26:
	fault = _lwr<EL>(inst, s);
	break;
      case 0x28:
	fault = _sb<EL>(inst, s);
	break;
      case 0x29:
	fault = _sh<EL>(inst, s);
	break;
      case 0x2a:
	fault = _swl<EL>(inst, s); 
	break;
      case 0x2b:
	fault = _sw<EL>(inst, s);
	break;	
      case 0x2e:
	fault = _swr<EL>(inst, s); 
	break;
      case 0x31:
	fault = _lwc1<EL>(inst, s);
	break;
      case 0x35:
	fault = _ldc1<EL>(inst, s);
	break;
      case 0x39:
	fault = _swc1<EL>(inst, s);
	break;
      case 0x3D:
	fault = _sdc1<EL>(inst, s);
	break;
      default:
	printf("%s: Unknown IType instruction (bits=%x) @ pc=0x%08x\n", 
	       __func__, inst, s ? s->pc : 0);
	exit(-1);
	break;
      }
    if(fault) {
      goto handle_exception;
    }
  }
  return;

 handle_exception:
  printf("got fault %x\n", fault);
  assert(false);
  
}
