#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "profileMips.hh"
#include "parseMips.hh"
#include "helper.hh"
#include "globals.hh"

static timeval32_t myTimeVal = {0,0};
static uint32_t myTime = 1<<20;

void execRType(uint32_t inst, state_t *s);
void execJType(uint32_t inst, state_t *s);
void execIType(uint32_t inst, state_t *s);
void execSpecial2(uint32_t inst, state_t *s);
void execSpecial3(uint32_t inst, state_t *s);
void execCoproc0(uint32_t inst, state_t *s);
void execCoproc1(uint32_t inst, state_t *s);
void execCoproc1x(uint32_t inst, state_t *s);
void execCoproc2(uint32_t inst, state_t *s);

static uint32_t getConditionCode(state_t *s, uint32_t cc);
static void setConditionCode(state_t *s, uint32_t v, uint32_t cc);

/* RType instructions */
static void _monitor(uint32_t inst, state_t *s);
static void _monitorBody(uint32_t inst, state_t *s);


/* IType instructions */
static void _beq(uint32_t inst, state_t *s);
static void _beql(uint32_t inst, state_t *s);
static void _bne(uint32_t inst, state_t *s);
static void _bnel(uint32_t inst, state_t *s);
static void _bgtz(uint32_t inst, state_t *s);
static void _bgtzl(uint32_t inst, state_t *s);
static void _blez(uint32_t inst, state_t *s);
static void _blezl(uint32_t inst, state_t *s);
static void _bgez_bltz(uint32_t inst, state_t *s);
static void _lw(uint32_t inst, state_t *s);
static void _lh(uint32_t inst, state_t *s);
static void _lb(uint32_t inst, state_t *s);
static void _lbu(uint32_t inst, state_t *s);
static void _lhu(uint32_t inst, state_t *s);
static void _ldc1(uint32_t inst, state_t *s);
static void _lwc1(uint32_t inst, state_t *s);

static void _sw(uint32_t inst, state_t *s);
static void _sh(uint32_t inst, state_t *s);
static void _sb(uint32_t inst, state_t *s);
static void _sdc1(uint32_t inst, state_t *s);
static void _swc1(uint32_t inst, state_t *s);

static void _mtc1(uint32_t inst, state_t *s);
static void _mfc1(uint32_t inst, state_t *s);


static void _lwl(uint32_t inst, state_t *s);
static void _lwr(uint32_t inst, state_t *s);
static void _swl(uint32_t inst, state_t *s);
static void _swr(uint32_t inst, state_t *s);

static void _sc(uint32_t inst, state_t *s);

/* FLOATING-POINT */
static void _c(uint32_t inst, state_t *s);
static void _cs(uint32_t inst, state_t *s);
static void _cd(uint32_t inst, state_t *s);

static void _cvts(uint32_t inst, state_t *s);
static void _cvtd(uint32_t inst, state_t *s);

static void _truncw(uint32_t inst, state_t *s);

static void _movci(uint32_t inst, state_t *s);

static void _fabs(uint32_t inst, state_t *s);
static void _fadd(uint32_t inst, state_t *s);
static void _fsub(uint32_t inst, state_t *s);
static void _fmul(uint32_t inst, state_t *s);
static void _fdiv(uint32_t inst, state_t *s);
static void _fmov(uint32_t inst, state_t *s);
static void _fneg(uint32_t inst, state_t *s);
static void _fsqrt(uint32_t inst, state_t *s);
static void _frsqrt(uint32_t inst, state_t *s);
static void _frecip(uint32_t inst, state_t *s);
static void _fmovc(uint32_t inst, state_t *s);
static void _fmovn(uint32_t inst, state_t *s);
static void _fmovz(uint32_t inst, state_t *s);

static void _abss(uint32_t inst, state_t *s);
static void _adds(uint32_t inst, state_t *s);
static void _subs(uint32_t inst, state_t *s);
static void _muls(uint32_t inst, state_t *s);
static void _divs(uint32_t inst, state_t *s);
static void _sqrts(uint32_t inst, state_t *s);
static void _rsqrts(uint32_t inst, state_t *s);
static void _negs(uint32_t inst, state_t *s);
static void _recips(uint32_t inst, state_t *s);
static void _movcs(uint32_t inst, state_t *s);

static void _absd(uint32_t inst, state_t *s);
static void _addd(uint32_t inst, state_t *s);
static void _subd(uint32_t inst, state_t *s);
static void _muld(uint32_t inst, state_t *s);
static void _divd(uint32_t inst, state_t *s);
static void _sqrtd(uint32_t inst, state_t *s);
static void _rsqrtd(uint32_t inst, state_t *s);
static void _negd(uint32_t inst, state_t *s);
static void _recipd(uint32_t inst, state_t *s);
static void _movcd(uint32_t inst, state_t *s);

static void _bc1f(uint32_t inst, state_t *s);
static void _bc1t(uint32_t inst, state_t *s);
static void _bc1fl(uint32_t inst, state_t *s);
static void _bc1tl(uint32_t inst, state_t *s);

static void _movd(uint32_t inst, state_t *s);
static void _movs(uint32_t inst, state_t *s);
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


void mkMonitorVectors(state_t *s) {
  for (uint32_t loop = 0; (loop < IDT_MONITOR_SIZE); loop += 4) {
      uint32_t vaddr = IDT_MONITOR_BASE + loop;
      uint32_t insn = (RSVD_INSTRUCTION |
		       (((loop >> 2) & RSVD_INSTRUCTION_ARG_MASK)
			<< RSVD_INSTRUCTION_ARG_SHIFT));
      *(uint32_t*)(s->mem+vaddr) = accessBigEndian(insn);
  }
}

void execMips(state_t *s) {
  uint8_t *mem = s->mem;
  uint32_t inst = accessBigEndian(*(uint32_t*)(mem + s->pc));
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
	s->gpr[rd] = s->gpr[rt] << sa;
	s->pc += 4;
	break;
      case 0x01: /* movci */
	_movci(inst,s);
	break;
      case 0x02: /* srl */
	s->gpr[rd] = ((uint32_t)s->gpr[rt] >> sa);
	s->pc += 4;
	break;
      case 0x03: /* sra */
	s->gpr[rd] = s->gpr[rt] >> sa;
	s->pc += 4;
	break;
      case 0x04: /* sllv */
	s->gpr[rd] = s->gpr[rt] << (s->gpr[rs] & 0x1f);
	s->pc += 4;
	break;
      case 0x05:
	_monitor(inst,s);
	break;
      case 0x06:  
	s->gpr[rd] = ((uint32_t)s->gpr[rt]) >> (s->gpr[rs] & 0x1f);
	s->pc += 4;
	break;
      case 0x07:  
	s->gpr[rd] = s->gpr[rt] >> (s->gpr[rs] & 0x1f);
	s->pc += 4;
	break;
      case 0x08: { /* jr */
	uint32_t jaddr = s->gpr[rs];
	s->pc += 4;
	execMips(s);
	s->pc = jaddr;
	break;
      }
      case 0x09: { /* jalr */
	uint32_t jaddr = s->gpr[rs];
	s->gpr[31] = s->pc+8;
	s->pc += 4;
	execMips(s);
	s->pc = jaddr;
	break;
      }
      case 0x0C: /* syscall */
	printf("syscall()\n");
	exit(-1);
	break;
      case 0x0D: /* break */
	s->brk = 1;
	break;
      case 0x0f: /* sync */
	s->pc += 4;
	break;
      case 0x10: /* mfhi */
	s->gpr[rd] = s->hi;
	s->pc += 4;
	break;
      case 0x11: /* mthi */ 
	s->hi = s->gpr[rs];
	s->pc += 4;
	break;
      case 0x12: /* mflo */
	s->gpr[rd] = s->lo;
	s->pc += 4;
	break;
      case 0x13: /* mtlo */
	s->lo = s->gpr[rs];
	s->pc += 4;
	break;
      case 0x18: { /* mult */
	int64_t y;
	y = (int64_t)s->gpr[rs] * (int64_t)s->gpr[rt];
	s->lo = (int32_t)(y & 0xffffffff);
	s->hi = (int32_t)(y >> 32);
	s->pc += 4;
	break;
      }
      case 0x19: { /* multu */
	uint64_t y;
	uint64_t u0 = (uint64_t)*((uint32_t*)&s->gpr[rs]);
	uint64_t u1 = (uint64_t)*((uint32_t*)&s->gpr[rt]);
	y = u0*u1;
	*((uint32_t*)&(s->lo)) = (uint32_t)y;
	*((uint32_t*)&(s->hi)) = (uint32_t)(y>>32);
	s->pc += 4;
	break;
      }
      case 0x1A: /* div */
	if(s->gpr[rt] != 0) {
	  s->lo = s->gpr[rs] / s->gpr[rt];
	  s->hi = s->gpr[rs] % s->gpr[rt];
	}
	s->pc += 4;
	break;
      case 0x1B: /* divu */
	if(s->gpr[rt] != 0) {
	  s->lo = (uint32_t)s->gpr[rs] / (uint32_t)s->gpr[rt];
	  s->hi = (uint32_t)s->gpr[rs] % (uint32_t)s->gpr[rt];
	}
	s->pc += 4;
	break;
      case 0x20: /* add */
	s->gpr[rd] = s->gpr[rs] + s->gpr[rt];
	s->pc += 4;
	break;
      case 0x21: { /* addu */
	uint32_t u_rs = (uint32_t)s->gpr[rs];
	uint32_t u_rt = (uint32_t)s->gpr[rt];
	s->gpr[rd] = u_rs + u_rt;
	s->pc += 4;
	break;
      }
      case 0x22: /* sub */
	printf("sub()\n");
	exit(-1);
	break;
      case 0x23:{ /*subu*/  
	uint32_t u_rs = (uint32_t)s->gpr[rs];
	uint32_t u_rt = (uint32_t)s->gpr[rt];
	uint32_t y = u_rs - u_rt;
	s->gpr[rd] = y;
	s->pc += 4;
	break;
      }
      case 0x24: /* and */
	s->gpr[rd] = s->gpr[rs] & s->gpr[rt];
	s->pc += 4;
	break;
      case 0x25: /* or */
	s->gpr[rd] = s->gpr[rs] | s->gpr[rt];
	s->pc += 4;
	break;
      case 0x26: /* xor */
	s->gpr[rd] = s->gpr[rs] ^ s->gpr[rt];
	s->pc += 4;
	break;
      case 0x27: /* nor */
	s->gpr[rd] = ~(s->gpr[rs] | s->gpr[rt]);
	s->pc += 4;
	break;
      case 0x2A: /* slt */
	s->gpr[rd] = s->gpr[rs] < s->gpr[rt];
	s->pc += 4;
	break;
      case 0x2B: { /* sltu */
	uint32_t urs = (uint32_t)s->gpr[rs];
	uint32_t urt = (uint32_t)s->gpr[rt];
	s->gpr[rd] = (urs < urt);
	s->pc += 4;
	break;
      }
      case 0x0B: /* movn */
	s->gpr[rd] = (s->gpr[rt] != 0) ? s->gpr[rs] : s->gpr[rd];
	s->pc +=4;
	break;
      case 0x0A: /* movz */
	s->gpr[rd] = (s->gpr[rt] == 0) ? s->gpr[rs] : s->gpr[rd];
	s->pc += 4;
	break;
      case 0x34: /* teq */
	if(s->gpr[rs] == s->gpr[rt]) {
	  printf("teq trap!!!!!\n");
	  exit(-1);
	}
	s->pc += 4;
	break;
      default:
	printf("%sunknown RType instruction %x, funct = %d%s\n", 
	       KRED, s->pc, funct, KNRM);
	exit(-1);
	break;
      }
  }
  else if(isSpecial2)
    execSpecial2(inst,s);
  else if(isSpecial3)
    execSpecial3(inst,s);
  else if(isJType) {
    uint32_t jaddr = inst & ((1<<26)-1);
    jaddr <<= 2;
    if(opcode==0x2) { /* j */
      s->pc += 4;
    }
    else if(opcode==0x3) { /* jal */
      s->gpr[31] = s->pc+8;
      s->pc += 4;
    }
    else {
      printf("Unknown JType instruction\n");
      exit(-1);
    }
    jaddr |= (s->pc & (~((1<<28)-1)));
    execMips(s);
    s->pc = jaddr;
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
    execCoproc1(inst,s);
  else if(isCoproc1x)
    execCoproc1x(inst,s);
  else if(isCoproc2) {
    printf("coproc2 unimplemented\n");  exit(-1);
  }
  else if(isLoadLinked)
    _lw(inst, s);
  else if(isStoreCond)
    _sc(inst, s);
  else { /* itype */
    uint32_t uimm32 = inst & ((1<<16) - 1);
    int16_t simm16 = (int16_t)uimm32;
    int32_t simm32 = (int32_t)simm16;
    switch(opcode) 
      {
      case 0x01:
	_bgez_bltz(inst, s); 
	break;
      case 0x04:
	_beq(inst, s); 
	break;
      case 0x05:
	_bne(inst, s); 
	break;
      case 0x06:
	_blez(inst, s); 
	break;
      case 0x07:
	_bgtz(inst, s); 
	break;
      case 0x08: /* addi */
	s->gpr[rt] = s->gpr[rs] + simm32;  
	s->pc+=4;
	break;
      case 0x09: /* addiu */
	s->gpr[rt] = s->gpr[rs] + simm32;  
	s->pc+=4;
	break;
      case 0x0A: /* slti */
	s->gpr[rt] = (s->gpr[rs] < simm32);
	s->pc += 4;
	break;
      case 0x0B:/* sltiu */
	s->gpr[rt] = ((uint32_t)s->gpr[rs] < (uint32_t)simm32);
	s->pc += 4;
	break;
      case 0x0c: /* andi */
	s->gpr[rt] = s->gpr[rs] & uimm32;
	s->pc += 4;
	break;
      case 0x0d: /* ori */
	s->gpr[rt] = s->gpr[rs] | uimm32;
	s->pc += 4;
	break;
      case 0x0e: /* xori */
	s->gpr[rt] = s->gpr[rs] ^ uimm32;
	s->pc += 4;
	break;
      case 0x0F: /* lui */
	uimm32 <<= 16;
	s->gpr[rt] = uimm32;
	s->pc += 4;
	break;
      case 0x14:
	_beql(inst, s); 
	break;
      case 0x16:
	_blezl(inst, s);
	break;
      case 0x15:
	_bnel(inst, s); 
	break;
      case 0x17:
	_bgtzl(inst, s); 
	break;
      case 0x20:
	_lb(inst, s);
	break;
      case 0x21:
	_lh(inst, s);
	break;
      case 0x22: 
	_lwl(inst, s);
	break;
      case 0x23:
	_lw(inst, s); 
	break;
      case 0x24:
	_lbu(inst, s);
	break;
      case 0x25:
	_lhu(inst, s);
	break;
      case 0x26:
	_lwr(inst, s);
	break;
      case 0x28:
	_sb(inst, s); 
	break;
      case 0x29:
	_sh(inst, s); 
	break;
      case 0x2a:
	_swl(inst, s); 
	break;
      case 0x2B:
	_sw(inst, s); 
	break;
      case 0x2e:
	_swr(inst, s); 
	break;
      case 0x31:
	_lwc1(inst, s);
	break;  
      case 0x35:
	_ldc1(inst, s);
	break;
      case 0x39:
	_swc1(inst, s);
	break;
      case 0x3D:
	_sdc1(inst, s);
	break;
      default:
	printf("%s: Unknown IType instruction (bits=%x) @ pc=0x%08x\n", 
	       __func__, inst, s ? s->pc : 0);
	exit(-1);
	break;
      }
  }
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

void execSpecial3(uint32_t inst,state_t *s)
{
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

void execCoproc1(uint32_t inst, state_t *s)
{
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
	  _bc1f(inst, s);
	  break;
	case 0x1:
	  _bc1t(inst, s);
	  break;
	case 0x2:
	  _bc1fl(inst, s);
	  break;
	case 0x3:
	  _bc1tl(inst, s);
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
      else
	{
	  switch(lowop)
	    {
	    case 0x0:
	      _fadd(inst, s);
	      break;
	    case 0x1:
	      _fsub(inst, s);
	      break;
	    case 0x2:
	      _fmul(inst, s);
	      break;
	    case 0x3:
	      _fdiv(inst, s);
	      break;
	    case 0x4:
	      _fsqrt(inst, s);
	      break;
	    case 0x5:
	      _fabs(inst, s);
	      break;
	    case 0x6:
	      _fmov(inst, s);
	      break;
	    case 0x7:
	      _fneg(inst, s);
	      break;
	    case 0x9:
	      /* todo : implement _truncl */
	      die();
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
	      _frecip(inst, s);
	      break;
	    case 0x16:
	      _frsqrt(inst, s);
	      break;
	    case 0x20:
	      /* cvt.s */
	      _cvts(inst, s);
	      break;
	    case 0x21:
	      _cvtd(inst, s);
	      break;
	    default:
	      printf("unhandled coproc1 instruction (%x) @ %08x\n", inst, s->pc);
	      exit(-1);
	      break;
	    }
	}
    }
}


template <typename T>
struct c1xExec {
  void operator()(coproc1x_t insn, state_t *s) {
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


template <typename T>
void lxc1(uint32_t inst, state_t *s) {
  mips_t mi(inst);
  uint32_t ea = s->gpr[mi.lc1x.base] + s->gpr[mi.lc1x.index];
  *reinterpret_cast<T*>(s->cpr1 + mi.lc1x.fd) = accessBigEndian(*reinterpret_cast<T*>(s->mem + ea));
  s->pc += 4;
}

void execCoproc1x(uint32_t inst, state_t *s) {
  mips_t mi(inst);

  switch(mi.lc1x.id)
    {
    case 0:
      //lwxc1
      lxc1<int32_t>(inst, s);
      return;
    case 1:
      //ldxc1
      lxc1<int64_t>(inst, s);
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


static void _beq(uint32_t inst, state_t *s) {
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  bool takeBranch = (s->gpr[rt] == s->gpr[rs]);

  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
  uint32_t npc = s->pc+4; 

  /* execute branch delay */
  s->pc +=4;
  execMips(s);
  if(takeBranch)
    {
      s->pc = (imm+npc);
    }
}

static void _beql(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  bool takeBranch = (s->gpr[rt] == s->gpr[rs]);
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
  uint32_t npc = s->pc+4; 
  s->pc +=4;

  /* execute branch delay */
  if(takeBranch)
    {
      execMips(s);
      s->pc = (imm+npc);
    }
  else
    s->pc += 4;
}

static void _bne(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  bool takeBranch = (s->gpr[rt] != s->gpr[rs]);
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;

  uint32_t npc = s->pc+4; 
  
  /* execute branch delay */
  s->pc +=4;
  execMips(s);

  if(takeBranch)
    s->pc = (imm+npc);
}



static void _bnel(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  bool takeBranch = (s->gpr[rt] != s->gpr[rs]);
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
  uint32_t npc = s->pc+4; 
  s->pc +=4;
  
  /* execute branch delay */
  if(takeBranch)
    {
      execMips(s);
      s->pc = (imm+npc);
    }
  else
    s->pc += 4;
}

static void _bgtz(uint32_t inst, state_t *s) {
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
  int32_t npc = s->pc+4; 
  bool takeBranch = (s->gpr[rs]>0);
  s->pc += 4;
  execMips(s);
  if(takeBranch)
    s->pc = imm+npc;
}

static void _bgtzl(uint32_t inst, state_t *s) {
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
  int32_t npc = s->pc+4; 
  bool takeBranch = (s->gpr[rs]>0);
  s->pc +=4;

  if(takeBranch) {
    execMips(s);
    s->pc = (imm+npc);
  }
  else 
    s->pc += 4;
}

static void _blezl(uint32_t inst, state_t *s)
{
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
  int32_t npc = s->pc+4; 
  bool takeBranch = (s->gpr[rs]<=0);
  s->pc +=4;

  if(takeBranch)
    {
      execMips(s);
      s->pc = (imm+npc);
    }
  else
    s->pc += 4;
  
}

static void _blez(uint32_t inst, state_t *s) {
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
  int32_t npc = s->pc+4; 
  bool takeBranch = (s->gpr[rs]<=0);
  s->pc += 4;
  execMips(s);
  if(takeBranch)
    s->pc = imm+npc;

  
}

static void _bgez_bltz(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
  int32_t npc = s->pc+4; 
  bool takeBranch = false;
  if(rt==0) {
    /* bltz : less than zero */
    takeBranch = (s->gpr[rs] < 0);
    s->pc += 4;
    execMips(s);
    if(takeBranch)
      s->pc = imm+npc;
  }
  else if(rt==1) {
    /* bgez : greater than or equal to zero */
    takeBranch = (s->gpr[rs] >= 0);
    s->pc += 4;
    execMips(s);
    if(takeBranch)
      s->pc = imm+npc;
  }
  else if(rt==2) {
    takeBranch = (s->gpr[rs] < 0);
    s->pc += 4;
    if(takeBranch) {
      execMips(s);
      s->pc = imm+npc;
    }
    else 
      s->pc += 4;
  }
  else if(rt == 3) {
    /* greater than zero likely */
    takeBranch = (s->gpr[rs] >=0);
    s->pc += 4;
    if(takeBranch) {
      execMips(s);
      s->pc = imm+npc;
    }
    else 
      s->pc += 4;
  }
}



static void _lw(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  uint32_t ea = (uint32_t)s->gpr[rs] + imm;

  s->gpr[rt] = accessBigEndian(*((int32_t*)(s->mem + ea))); 
  s->pc += 4;
}

static void _lh(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  
  uint32_t ea = s->gpr[rs] + imm;
  int16_t mem = accessBigEndian(*((int16_t*)(s->mem + ea)));
  s->gpr[rt] = (int32_t)mem;
  s->pc +=4;
}


static void _lb(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  
  uint32_t ea = s->gpr[rs] + imm;
  int8_t v = *((int8_t*)(s->mem + ea));
  s->gpr[rt] = (int32_t)v;
  s->pc += 4;
}

static void _lbu(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  
  uint32_t ea = s->gpr[rs] + imm;
  uint32_t zExt = (uint32_t)s->mem[ea];
  *((uint32_t*)&(s->gpr[rt])) = zExt;
  s->pc += 4;
}


static void _lhu(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  
  uint32_t ea = s->gpr[rs] + imm;
  uint32_t zExt = accessBigEndian(*((uint16_t*)(s->mem + ea)));
  *((uint32_t*)&(s->gpr[rt])) = zExt;
  s->pc += 4;
}

static void _sc(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  _sw(inst, s);
  s->gpr[rt] = 1;
}


static void _sw(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  uint32_t ea = s->gpr[rs] + imm;
  *((int32_t*)(s->mem + ea)) = accessBigEndian(s->gpr[rt]);
  s->pc += 4;
}

static void _sh(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
    
  uint32_t ea = s->gpr[rs] + imm;
  *((int16_t*)(s->mem + ea)) = accessBigEndian(((int16_t)s->gpr[rt]));
  s->pc += 4;
}

static void _sb(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
    
  uint32_t ea = s->gpr[rs] + imm;
  s->mem[ea] = (uint8_t)s->gpr[rt];
  s->pc +=4;
}

static void _mtc1(uint32_t inst, state_t *s)
{
  uint32_t rd = (inst>>11) & 31;
  uint32_t rt = (inst>>16) & 31;
  s->cpr1[rd] = s->gpr[rt];
  s->pc += 4;
}

static void _mfc1(uint32_t inst, state_t *s)
{
  uint32_t rd = (inst>>11) & 31;
  uint32_t rt = (inst>>16) & 31;
  s->gpr[rt] = s->cpr1[rd];
  s->pc +=4;
}




static void _monitor(uint32_t inst, state_t *s){
  _monitorBody(inst, s);
}

static void _swl(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;

  uint32_t ea = s->gpr[rs] + imm;
  uint32_t ma = ea & 3;
  ea &= 0xfffffffc;
#ifdef MIPSEL
  ma = 3 - ma;
#endif
  uint32_t r = accessBigEndian(*((int32_t*)(s->mem + ea))); 
  uint32_t xx=0,x = s->gpr[rt];
  
  uint32_t xs = x >> (8*ma);
  uint32_t m = ~((1U << (8*(4 - ma))) - 1);
  xx = (r & m) | xs;
  *((uint32_t*)(s->mem + ea)) = accessBigEndian(xx);
  s->pc += 4;
}

static void _swr(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
   
  uint32_t ea = s->gpr[rs] + imm;
  uint32_t ma = ea & 3;
#ifdef MIPSEL
  ma = 3 - ma;
#endif
  ea &= 0xfffffffc;
  uint32_t r = accessBigEndian(*((int32_t*)(s->mem + ea))); 
  uint32_t xx=0,x = s->gpr[rt];
  
  uint32_t xs = 8*(3-ma);
  uint32_t rm = (1U << xs) - 1;

  xx = (x << xs) | (rm & r);
  *((uint32_t*)(s->mem + ea)) = accessBigEndian(xx);
  s->pc += 4;
}

static void _lwl(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  
  uint32_t ea = ((uint32_t)s->gpr[rs] + imm);
  uint32_t ma = ea & 3;
  ea &= 0xfffffffc;
#ifdef MIPSEL
  ma = 3 - ma;
#endif
  int32_t r = accessBigEndian(*((int32_t*)(s->mem + ea))); 
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
}

static void _lwr(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
 
  uint32_t ea = ((uint32_t)s->gpr[rs] + imm);
  uint32_t ma = ea & 3;
  ea &= 0xfffffffc;
#ifdef MIPSEL
  ma = 3-ma;
#endif
  uint32_t r = accessBigEndian(*((int32_t*)(s->mem + ea))); 
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
}

static void _monitorBody(uint32_t inst, state_t *s)
{
 uint32_t reason = (inst >> RSVD_INSTRUCTION_ARG_SHIFT) & RSVD_INSTRUCTION_ARG_MASK;
  reason >>= 1;
  int32_t fd=-1,nr=-1,flags=-1;
  char *path;
  struct timeval tp;
  timeval32_t tp32;
  struct tms tms_buf;
  tms32_t tms32_buf;
  struct stat native_stat;
  stat32_t *host_stat = nullptr;

  switch(reason)
    {
    case 6: /* int open(char *path, int flags) */
      path = (char*)(s->mem + (uint32_t)s->gpr[R_a0]);
      flags = remapIOFlags(s->gpr[R_a1]);
      fd = open(path, flags, S_IRUSR|S_IWUSR);
      s->gpr[R_v0] = fd;
      break;
    case 7: /* int read(int file,char *ptr,int len) */
      fd = s->gpr[R_a0];
      nr = s->gpr[R_a2];
      s->gpr[R_v0] = read(fd, (char*)(s->mem + (uint32_t)s->gpr[R_a1]), nr);
      break;
    case 8: 
      /* int write(int file, char *ptr, int len) */
      fd = s->gpr[R_a0];
      nr = s->gpr[R_a2];
      s->gpr[R_v0] = (int32_t)write(fd, (void*)(s->mem + (uint32_t)s->gpr[R_a1]), nr);
      if(fd==1)
	fflush(stdout);
      else if(fd==2)
	fflush(stderr);
      break;
    case 9:
      /* off_t lseek(int fd, off_t offset, int whence); */
      s->gpr[R_v0] = lseek(s->gpr[R_a0], s->gpr[R_a1], s->gpr[R_a2]);
      break;
    case 10:
      fd = s->gpr[R_a0];
      if(fd>2)
	s->gpr[R_v0] = (int32_t)close(fd);
      else
	s->gpr[R_v0] = 0;
      break;
    case 13:
      /* fstat */
      fd = s->gpr[R_a0];
      s->gpr[R_v0] = fstat(fd, &native_stat);
      host_stat = (stat32_t*)(s->mem + (uint32_t)s->gpr[R_a1]); 

      host_stat->st_dev = accessBigEndian((uint32_t)native_stat.st_dev);
      host_stat->st_ino = accessBigEndian((uint16_t)native_stat.st_ino);
      host_stat->st_mode = accessBigEndian((uint32_t)native_stat.st_mode);
      host_stat->st_nlink = accessBigEndian((uint16_t)native_stat.st_nlink);
      host_stat->st_uid = accessBigEndian((uint16_t)native_stat.st_uid);
      host_stat->st_gid = accessBigEndian((uint16_t)native_stat.st_gid);
      host_stat->st_size = accessBigEndian((uint32_t)native_stat.st_size);
      host_stat->_st_atime = accessBigEndian((uint32_t)native_stat.st_atime);
      host_stat->_st_mtime = 0;
      host_stat->_st_ctime = 0;
      host_stat->st_blksize = accessBigEndian((uint32_t)native_stat.st_blksize);
      host_stat->st_blocks = accessBigEndian((uint32_t)native_stat.st_blocks);

      break;
    case 33:
      if(enClockFuncts) {
	gettimeofday(&tp, nullptr);
	tp32.tv_sec = accessBigEndian((uint32_t)tp.tv_sec);
	tp32.tv_usec = accessBigEndian((uint32_t)tp.tv_usec);
      } else {
	memcpy(&tp32, &myTimeVal, sizeof(tp32));
	myTimeVal.tv_usec++;
	if(myTimeVal.tv_usec == (1<<20))
	  {
	    myTimeVal.tv_usec = 0;
	    myTimeVal.tv_sec++;
	  }
      }
      *((timeval32_t*)(s->mem + (uint32_t)s->gpr[R_a0] + 0)) = tp32;
      s->gpr[R_v0] = 0;
      break;
    case 34:
      if(enClockFuncts) {
	*((uint32_t*)(&s->gpr[R_v0])) = times(&tms_buf);
	tms32_buf.tms_utime = accessBigEndian((uint32_t)tms_buf.tms_utime);
	tms32_buf.tms_stime = accessBigEndian((uint32_t)tms_buf.tms_stime);
	tms32_buf.tms_cutime = accessBigEndian((uint32_t)tms_buf.tms_cutime);
	tms32_buf.tms_cstime = accessBigEndian((uint32_t)tms_buf.tms_cstime);
      } else {
	*((uint32_t*)(&s->gpr[R_v0])) = myTime;
	myTime += 100;
	memset(&tms32_buf, 0, sizeof(tms32_buf));
      }
      *((tms32_t*)(s->mem + (uint32_t)s->gpr[R_a0] + 0)) = tms32_buf;
      break;
    case 35:
      /* int getargs(char **argv) */
      for(int i = 0; i < std::min(MARGS, sysArgc); i++)
	{
	  uint32_t arrayAddr = ((uint32_t)s->gpr[R_a0])+4*i;
	  uint32_t ptr = accessBigEndian(*((uint32_t*)(s->mem + arrayAddr)));
	  strcpy((char*)(s->mem + ptr), sysArgv[i]);
	}
      s->gpr[R_v0] = sysArgc;
      break;
    case 37:
      /*char *getcwd(char *buf, uint32_t size) */
      path = (char*)(s->mem + (uint32_t)s->gpr[R_a0]);
      getcwd(path, (uint32_t)s->gpr[R_a1]);
      s->gpr[R_v0] = s->gpr[R_a0];
      break;
    case 38:
      /* int chdir(const char *path); */
      path = (char*)(s->mem + (uint32_t)s->gpr[R_a0]);
      //printf("chdir(%s)\n", path);
      s->gpr[R_v0] = chdir(path);
      break;
    case 55: 
      /* void get_mem_info(unsigned int *ptr) */
      /* in:  A0 = pointer to three word memory location */
      /* out: [A0 + 0] = size */
      /*      [A0 + 4] = instruction cache size */
      /*      [A0 + 8] = data cache size */
      /* 256 MBytes of DRAM */
      *((uint32_t*)(s->mem + (uint32_t)s->gpr[R_a0] + 0)) = accessBigEndian(K1SIZE);
      /* No Icache */
      *((uint32_t*)(s->mem + (uint32_t)s->gpr[R_a0] + 4)) = 0;
      /* No Dcache */
      *((uint32_t*)(s->mem + (uint32_t)s->gpr[R_a0] + 8)) = 0;
      break;
    default:
      printf("unhandled monitor instruction (reason = %d)\n", reason);
      exit(-1);
      break;
    }
  s->pc = s->gpr[31];
}



static void _ldc1(uint32_t inst, state_t *s)
{
  uint32_t ft = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  uint32_t ea = s->gpr[rs] + imm;
  *((int64_t*)(s->cpr1 + ft)) = accessBigEndian(*((int64_t*)(s->mem + ea))); 
  s->pc += 4;
}
static void _sdc1(uint32_t inst, state_t *s)
{
  uint32_t ft = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  uint32_t ea = s->gpr[rs] + imm;
  *((int64_t*)(s->mem + ea)) = accessBigEndian((*(int64_t*)(s->cpr1 + ft)));
  s->pc += 4;
}
static void _lwc1(uint32_t inst, state_t *s)
{
  uint32_t ft = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  uint32_t ea = s->gpr[rs] + imm;
  uint32_t v = accessBigEndian(*((uint32_t*)(s->mem + ea))); 
  *((float*)(s->cpr1 + ft)) = *((float*)&v);
  s->pc += 4;
}
static void _swc1(uint32_t inst, state_t *s)
{
  uint32_t ft = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  uint32_t ea = s->gpr[rs] + imm;
  uint32_t v = *((uint32_t*)(s->cpr1+ft));
  *((uint32_t*)(s->mem + ea)) = accessBigEndian(v);
  s->pc += 4;
}

/* normal versions */
static void _bc1f(uint32_t inst, state_t *s) {
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
  int32_t npc = s->pc+4; 
  uint32_t cc = (inst >> 18) & 7;
  bool takeBranch = getConditionCode(s,cc)==0;
  s->pc += 4;
  execMips(s);
  if(takeBranch)
    s->pc = imm+npc;
  
}
static void _bc1t(uint32_t inst, state_t *s) { 
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
  int32_t npc = s->pc+4; 
  uint32_t cc = (inst >> 18) & 7;
  bool takeBranch = getConditionCode(s,cc)==1;
  s->pc += 4;
  execMips(s);
  if(takeBranch)
    s->pc = (imm+npc);
  
}

static void _bc1fl(uint32_t inst, state_t *s)
{
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
  int32_t npc = s->pc+4; 
  uint32_t cc = (inst >> 18) & 7;
  bool takeBranch = getConditionCode(s,cc)==0;

  s->pc +=4;

  if(takeBranch)
    {
      execMips(s);
      s->pc = (imm+npc);
    }
  else
    s->pc += 4;
}

static void _bc1tl(uint32_t inst, state_t *s)
{

  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
  int32_t npc = s->pc+4; 
  uint32_t cc = (inst >> 18) & 7;
  bool takeBranch = getConditionCode(s,cc)==1; 
  s->pc +=4;

  if(takeBranch)
    {
      
      execMips(s);
      s->pc = (imm+npc);
    }
  else
    s->pc += 4;
}


static void _truncw(uint32_t inst, state_t *s)
{
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

static void _movd(uint32_t inst, state_t *s) {
  uint32_t fd = (inst>>6) & 31;
  uint32_t fs = (inst>>11) & 31;
  s->cpr1[fd+0] = s->cpr1[fs+0];
  s->cpr1[fd+1] = s->cpr1[fs+1];
  s->pc += 4;
}

static void _movs(uint32_t inst, state_t *s)
{
  uint32_t fd = (inst>>6) & 31;
  uint32_t fs = (inst>>11) & 31;
  s->cpr1[fd+0] = s->cpr1[fs+0];
  s->pc += 4;
}

static void _movnd(uint32_t inst, state_t *s)
{
  uint32_t fd = (inst>>6) & 31;
  uint32_t fs = (inst>>11) & 31;
  uint32_t rt = (inst>>16) & 31;
  bool notZero = (s->gpr[rt] != 0);
  s->cpr1[fd+0] = notZero ? s->cpr1[fs+0] : s->cpr1[fd+0];
  s->cpr1[fd+1] = notZero ? s->cpr1[fs+1] : s->cpr1[fd+1];
  s->pc += 4;
}

static void _movns(uint32_t inst, state_t *s)
{
  uint32_t fd = (inst>>6) & 31;
  uint32_t fs = (inst>>11) & 31;
  uint32_t rt = (inst>>16) & 31;
  bool notZero = (s->gpr[rt] != 0);
  s->cpr1[fd+0] = notZero ? s->cpr1[fs+0] : s->cpr1[fd+0];
  s->pc += 4;
}

static void _movzd(uint32_t inst, state_t *s)
{
  uint32_t fd = (inst>>6) & 31;
  uint32_t fs = (inst>>11) & 31;
  uint32_t rt = (inst>>16) & 31;
 
  s->cpr1[fd+0] = (s->gpr[rt] == 0) ? s->cpr1[fs+0] : s->cpr1[fd+0];
  s->cpr1[fd+1] = (s->gpr[rt] == 0) ? s->cpr1[fs+1] : s->cpr1[fd+1];
  s->pc += 4;
}

static void _movzs(uint32_t inst, state_t *s)
{
  uint32_t fd = (inst>>6) & 31;
  uint32_t fs = (inst>>11) & 31;
  uint32_t rt = (inst>>16) & 31;

  s->cpr1[fd+0] = (s->gpr[rt] == 0) ? s->cpr1[fs+0] : s->cpr1[fd+0];
  s->pc += 4;
}

static void _movcd(uint32_t inst, state_t *s)
{
  uint32_t cc = (inst >> 18) & 7;
  uint32_t fd = (inst>>6) & 31;
  uint32_t fs = (inst>>11) & 31;
  uint32_t tf = (inst>>16) & 1;

  if(tf==0)
    {
      if(getConditionCode(s,cc)==0) {
	s->cpr1[fd+0] = s->cpr1[fs+0];
	s->cpr1[fd+1] = s->cpr1[fs+1];
      }
    }
  else
    {
      if(getConditionCode(s,cc)==1) {
	s->cpr1[fd+0] = s->cpr1[fs+0];
	s->cpr1[fd+1] = s->cpr1[fs+1];
      }
    }

  s->pc += 4;
}

static void _movcs(uint32_t inst, state_t *s)
{
  uint32_t cc = (inst >> 18) & 7;
  uint32_t fd = (inst>>6) & 31;
  uint32_t fs = (inst>>11) & 31;
  uint32_t tf = (inst>>16) & 1;
  if(tf==0)
    {
      s->cpr1[fd+0] = getConditionCode(s, cc) ? s->cpr1[fd+0] : s->cpr1[fs+0];
    }
  else
    {
      s->cpr1[fd+0] = getConditionCode(s, cc) ? s->cpr1[fs+0] : s->cpr1[fd+0];
    }
  s->pc += 4;
}


static void _movci(uint32_t inst, state_t *s)
{
  uint32_t cc = (inst >> 18) & 7;
  uint32_t tf = (inst>>16) & 1;
  uint32_t rd = (inst>>11) & 31;
  uint32_t rs = (inst >> 21) & 31;

  if(tf==0)
    {
      /* movf */
      s->gpr[rd] = getConditionCode(s, cc) ? s->gpr[rd] : s->gpr[rs];
    }
  else
    {
      /* movt */
      s->gpr[rd] = getConditionCode(s, cc) ? s->gpr[rs] : s->gpr[rd];
    }

  s->pc += 4;
}

static void _cvts(uint32_t inst, state_t *s)
{
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

static void _cvtd(uint32_t inst, state_t *s)
{
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



static void _fabs(uint32_t inst, state_t *s)
{
  uint32_t fmt = (inst >> 21) & 31;
  switch(fmt)
    {
    case FMT_S:
      _abss(inst, s);
      break;
    case FMT_D:
      _absd(inst, s);
      break;
    default:
      printf("unsupported %s\n", __func__);
      exit(-1);
      break;
    }
}


static void _fmov(uint32_t inst, state_t *s)
{
 uint32_t fmt = (inst >> 21) & 31;
  switch(fmt)
    {
    case FMT_S:
      _movs(inst, s);
      break;
    case FMT_D:
      _movd(inst, s);
      break;
    default:
      printf("unsupported %s\n", __func__);
      exit(-1);
      break;
    }
}

static void _fmovn(uint32_t inst, state_t *s)
{
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


static void _fmovz(uint32_t inst, state_t *s)
{
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

static void _fneg(uint32_t inst, state_t *s)
{
  uint32_t fmt = (inst >> 21) & 31;
  switch(fmt)
    {
    case FMT_S:
      _negs(inst, s);
      break;
    case FMT_D:
      _negd(inst, s);
      break;
    default:
      printf("unsupported %s\n", __func__);
      exit(-1);
      break;
    }
}



static void _fadd(uint32_t inst, state_t *s)
{
  uint32_t fmt = (inst >> 21) & 31;
  switch(fmt)
    {
    case FMT_S:
      _adds(inst, s);
      break;
    case FMT_D:
      _addd(inst, s);
      break;
    default:
      printf("unsupported add\n");
      exit(-1);
      break;
    }
}
static void _fsub(uint32_t inst, state_t *s)
{
  uint32_t fmt = (inst >> 21) & 31;
  switch(fmt)
    {
    case FMT_S:
      _subs(inst, s);
      break;
    case FMT_D:
      _subd(inst, s);
      break;
    default:
      printf("unsupported sub\n");
      exit(-1);
      break;
    }
}

static void _fsqrt(uint32_t inst, state_t *s)
{
  uint32_t fmt = (inst >> 21) & 31;
  switch(fmt)
    {
    case FMT_S:
      _sqrts(inst, s);
      break;
    case FMT_D:
      _sqrtd(inst, s);
      break;
    default:
      printf("unsupported %s\n", __func__);
      exit(-1);
      break;
    }
}

static void _frsqrt(uint32_t inst, state_t *s)
{
  uint32_t fmt = (inst >> 21) & 31;
  switch(fmt)
    {
    case FMT_S:
      _rsqrts(inst, s);
      break;
    case FMT_D:
      _rsqrtd(inst, s);
      break;
    default:
      printf("unsupported %s\n", __func__);
      exit(-1);
      break;
    }
}

static void _frecip(uint32_t inst, state_t *s)
{
  uint32_t fmt = (inst >> 21) & 31;
  switch(fmt)
    {
    case FMT_S:
      _recips(inst, s);
      break;
    case FMT_D:
      _recipd(inst, s);
      break;
    default:
      printf("unsupported %s\n", __func__);
      exit(-1);
      break;
    }
}

static void _fmovc(uint32_t inst, state_t *s)
{
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

static void _fmul(uint32_t inst, state_t *s)
{
  uint32_t fmt = (inst >> 21) & 31;
  switch(fmt)
    {
    case FMT_S:
      _muls(inst, s);
      break;
    case FMT_D:
      _muld(inst, s);
      break;
    default:
      printf("unsupported %s\n", __func__);
      exit(-1);
      break;
    }
}

static void _fdiv(uint32_t inst, state_t *s)
{
  uint32_t fmt = (inst >> 21) & 31;
  switch(fmt)
    {
    case FMT_S:
      _divs(inst, s);
      break;
    case FMT_D:
      _divd(inst, s);
      break;
    default:
      printf("unsupported %s\n", __func__);
      exit(-1);
      break;
    }
}

static void _abss(uint32_t inst, state_t *s)
{
  uint32_t fs = (inst >> 11) & 31;
  uint32_t fd = (inst >> 6) & 31;

  float f_fs = *((float*)(s->cpr1+fs));
  *((float*)(s->cpr1 + fd)) = f_fs < 0.0f ? -f_fs : f_fs;
  
  s->pc += 4;
}

static void _absd(uint32_t inst, state_t *s)
{
  uint32_t fs = (inst >> 11) & 31;
  uint32_t fd = (inst >> 6) & 31;

  double d_fs = *((double*)(s->cpr1+fs));
  *((double*)(s->cpr1 + fd)) = d_fs < 0.0 ? -d_fs : d_fs;
  s->pc += 4;
}

static void _recips(uint32_t inst, state_t *s)
{
  uint32_t fs = (inst >> 11) & 31;
  uint32_t fd = (inst >> 6) & 31;

  float f_fs = *((float*)(s->cpr1+fs));
  *((float*)(s->cpr1 + fd)) = 1.0 / f_fs;
  
  s->pc += 4;
}

static void _recipd(uint32_t inst, state_t *s)
{
  uint32_t fs = (inst >> 11) & 31;
  uint32_t fd = (inst >> 6) & 31;

  double d_fs = *((double*)(s->cpr1+fs));
  *((double*)(s->cpr1 + fd)) = 1.0 / d_fs;
  s->pc += 4;
}

static void _negs(uint32_t inst, state_t *s)
{
  uint32_t fs = (inst >> 11) & 31;
  uint32_t fd = (inst >> 6) & 31;

  float f_fs = *((float*)(s->cpr1+fs));
  *((float*)(s->cpr1 + fd)) = -f_fs;
  
  s->pc += 4;
}

static void _negd(uint32_t inst, state_t *s)
{
  uint32_t fs = (inst >> 11) & 31;
  uint32_t fd = (inst >> 6) & 31;

  double d_fs = *((double*)(s->cpr1+fs));
  *((double*)(s->cpr1 + fd)) = -d_fs;
  s->pc += 4;
}


static void _adds(uint32_t inst, state_t *s)
{
  uint32_t ft = (inst >> 16) & 31;
  uint32_t fs = (inst >> 11) & 31;
  uint32_t fd = (inst >> 6) & 31;

  float f_fs = *((float*)(s->cpr1+fs));
  float f_ft = *((float*)(s->cpr1+ft));
  *((float*)(s->cpr1 + fd)) = f_fs + f_ft;
  s->pc += 4;
}

static void _addd(uint32_t inst, state_t *s)
{
  uint32_t ft = (inst >> 16) & 31;
  uint32_t fs = (inst >> 11) & 31;
  uint32_t fd = (inst >> 6) & 31;

  double d_fs = *((double*)(s->cpr1+fs));
  double d_ft = *((double*)(s->cpr1+ft));
  *((double*)(s->cpr1 + fd)) = d_fs + d_ft;
  //printf("d_fs = %g, d_ft = %g, result = %g\n", d_fs, d_ft,
  //*((double*)(s->cpr1 + fd)) );
  s->pc += 4;
}

static void _subs(uint32_t inst, state_t *s)
{
  uint32_t ft = (inst >> 16) & 31;
  uint32_t fs = (inst >> 11) & 31;
  uint32_t fd = (inst >> 6) & 31;

  float f_fs = *((float*)(s->cpr1+fs));
  float f_ft = *((float*)(s->cpr1+ft));
  *((float*)(s->cpr1 + fd)) = f_fs - f_ft;
  
  s->pc += 4;
}

static void _subd(uint32_t inst, state_t *s)
{
  uint32_t ft = (inst >> 16) & 31;
  uint32_t fs = (inst >> 11) & 31;
  uint32_t fd = (inst >> 6) & 31;

  double d_fs = *((double*)(s->cpr1+fs));
  double d_ft = *((double*)(s->cpr1+ft));
  *((double*)(s->cpr1 + fd)) = d_fs - d_ft;
  s->pc += 4;
}

static void _muls(uint32_t inst, state_t *s)
{
  uint32_t ft = (inst >> 16) & 31;
  uint32_t fs = (inst >> 11) & 31;
  uint32_t fd = (inst >> 6) & 31;

  float f_fs = *((float*)(s->cpr1+fs));
  float f_ft = *((float*)(s->cpr1+ft));
  *((float*)(s->cpr1 + fd)) = f_fs * f_ft;
  
  s->pc += 4;
}

static void _muld(uint32_t inst, state_t *s)
{
  uint32_t ft = (inst >> 16) & 31;
  uint32_t fs = (inst >> 11) & 31;
  uint32_t fd = (inst >> 6) & 31;

  double d_fs = *((double*)(s->cpr1+fs));
  double d_ft = *((double*)(s->cpr1+ft));
  *((double*)(s->cpr1 + fd)) = d_fs * d_ft;
  s->pc += 4;
}

static void _sqrts(uint32_t inst, state_t *s)
{
  uint32_t fs = (inst >> 11) & 31;
  uint32_t fd = (inst >> 6) & 31;

  float f_fs = *((float*)(s->cpr1+fs));
  *((float*)(s->cpr1 + fd)) = sqrtf(f_fs);
  
  s->pc += 4;
}

static void _sqrtd(uint32_t inst, state_t *s)
{
  uint32_t fs = (inst >> 11) & 31;
  uint32_t fd = (inst >> 6) & 31;

  double d_fs = *((double*)(s->cpr1+fs));
  *((double*)(s->cpr1 + fd)) = sqrt(d_fs);
  s->pc += 4;
}

static void _rsqrts(uint32_t inst, state_t *s)
{
  uint32_t fs = (inst >> 11) & 31;
  uint32_t fd = (inst >> 6) & 31;

  float f_fs = *((float*)(s->cpr1+fs));
  *((float*)(s->cpr1 + fd)) = 1.0f / sqrtf(f_fs);
  
  s->pc += 4;
}

static void _rsqrtd(uint32_t inst, state_t *s)
{
  uint32_t fs = (inst >> 11) & 31;
  uint32_t fd = (inst >> 6) & 31;

  double d_fs = *((double*)(s->cpr1+fs));
  *((double*)(s->cpr1 + fd)) = 1.0 / sqrt(d_fs);
  s->pc += 4;
}

static void _divs(uint32_t inst, state_t *s)
{
  uint32_t ft = (inst >> 16) & 31;
  uint32_t fs = (inst >> 11) & 31;
  uint32_t fd = (inst >> 6) & 31;

  float f_fs = *((float*)(s->cpr1+fs));
  float f_ft = *((float*)(s->cpr1+ft));
  *((float*)(s->cpr1 + fd)) = f_fs / f_ft;
  
  s->pc += 4;
}

static void _divd(uint32_t inst, state_t *s)
{
  uint32_t ft = (inst >> 16) & 31;
  uint32_t fs = (inst >> 11) & 31;
  uint32_t fd = (inst >> 6) & 31;

  double d_fs = *((double*)(s->cpr1+fs));
  double d_ft = *((double*)(s->cpr1+ft));
  *((double*)(s->cpr1 + fd)) = d_fs / d_ft;
  s->pc += 4;
}



static void _c(uint32_t inst, state_t *s)
{
  uint32_t fmt = (inst >> 21) & 31;
  switch(fmt)
    {
    case FMT_S:
      _cs(inst, s);
      break;
    case FMT_D:
      _cd(inst, s);
      break;
    default:
      printf("unsupported comparison\n");
      exit(-1);
      break;
    }
}


static void _cs(uint32_t inst, state_t *s)
{
  uint32_t cond = inst & 15;
  uint32_t cc = (inst >> 8) & 7;
  uint32_t ft = (inst >> 16) & 31;
  uint32_t fs = (inst >> 11) & 31;
  float f_fs = *((float*)(s->cpr1+fs));
  float f_ft = *((float*)(s->cpr1+ft));
  uint32_t v = 0;

  switch(cond)
    {
      /*
    case COND_F:
      break;
      */
    case COND_UN:
      v = (f_fs == f_ft);
      setConditionCode(s,v,cc);
      break;
    case COND_EQ:
      v = (f_fs == f_ft);
      setConditionCode(s,v,cc);
      break;
      /*
    case COND_UEQ:
      break;
    case COND_OLT:
      break;
    case COND_ULT:
      break;
    case COND_OLE:
      break;
    case COND_ULE:
      break;
    case COND_SF:
      break;
    case COND_NGLE:
      break;
    case COND_SEQ:
      break;
    case COND_NGL:
      break;
      */
    case COND_LT:
      v = (f_fs < f_ft);
      setConditionCode(s,v,cc);
      break;
      /*
    case COND_NGE:
      break;
      */
    case COND_LE:
      v = (f_fs <= f_ft);
      setConditionCode(s,v,cc);
      break;
      /*
    case COND_NGT:
      break;
      */
    default:
      printf("unimplemented %s = %s\n", __func__, getCondName(cond).c_str());
      exit(-1);
      break;
    }
  s->pc += 4;
}

static void _cd(uint32_t inst, state_t *s)
{
  uint32_t cond = inst & 15;
  uint32_t cc = (inst >> 8) & 7;
  uint32_t ft = (inst >> 16) & 31;
  uint32_t fs = (inst >> 11) & 31;
  double d_fs = *((double*)(s->cpr1+fs));
  double d_ft = *((double*)(s->cpr1+ft));
  uint32_t v = 0;
  
  //printf("c.%d.d @ %x\n", cond, s->pc);

  switch(cond)
    {
      /*
    case COND_F:
      break;
      */
    case COND_UN:
      v = (d_fs == d_ft);
      setConditionCode(s,v,cc);
      //printf("pc = %x : d_fs = %g, d_ft = %g, eq=%d\n", 
      //s->pc, d_fs, d_ft, v); 
      //exit(-1);
 
      break;

    case COND_EQ:
      v = (d_fs == d_ft);
      //printf("pc = %x : d_fs = %g, d_ft = %g, eq=%d\n", 
      //s->pc, d_fs, d_ft, v); 
      setConditionCode(s,v,cc);
      break;
       /*
    case COND_UEQ:
      break;
    case COND_OLT:
      break;
    case COND_ULT:
      break;
    case COND_OLE:
      break;
    case COND_ULE:
      break;
    case COND_SF:
      break;
    case COND_NGLE:
      break;
    case COND_SEQ:
      break;
    case COND_NGL:
      break;
      */
    case COND_LT:
      v = (d_fs < d_ft);
      setConditionCode(s,v,cc);
      break;
      /*
    case COND_NGE:
      break;
      */
    case COND_LE:
      v = (d_fs <= d_ft);
      setConditionCode(s,v,cc);
      break;
      /*
    case COND_NGT:
      break;
      */
    default:
      printf("unimplemented %s = %s\n", __func__, getCondName(cond).c_str());
      exit(-1);
      break;
    }
  s->pc += 4;
}
