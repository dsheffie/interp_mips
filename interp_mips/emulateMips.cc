#include "emulateMips.h"
#include "parseMips.h"

#include "helper.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>

#include <unistd.h>

#define K1SIZE  (0x20000000)

extern std::string log;

void execRType(uint32_t inst, state_t *s);
void execJType(uint32_t inst, state_t *s);
void execIType(uint32_t inst, state_t *s);
void execSpecial2(uint32_t inst, state_t *s);
void execSpecial3(uint32_t inst, state_t *s);

void execCoproc0(uint32_t inst, state_t *s);
void execCoproc1(uint32_t inst, state_t *s);
void execCoproc2(uint32_t inst, state_t *s);

/* RType instructions */
static void _monitor(uint32_t inst, state_t *s);

static void _add(uint32_t inst, state_t *s);
static void _addu(uint32_t inst, state_t *s);

static void _and(uint32_t inst, state_t *s);
static void _break(uint32_t inst, state_t *s);
static void _div(uint32_t inst, state_t *s);
static void _divu(uint32_t inst, state_t *s);
static void _jalr(uint32_t inst, state_t *s);
static void _jr(uint32_t inst, state_t *s);
static void _movn(uint32_t inst, state_t *s);
static void _movz(uint32_t inst, state_t *s);

static void _mfhi(uint32_t inst, state_t *s);
static void _mflo(uint32_t inst, state_t *s);
static void _mthi(uint32_t inst, state_t *s);
static void _mtlo(uint32_t inst, state_t *s);
static void _mult(uint32_t inst, state_t *s);
static void _multu(uint32_t inst, state_t *s);

static void _mul(uint32_t inst, state_t *s);
static void _madd(uint32_t inst, state_t *s);

static void _nor(uint32_t inst, state_t *s);
static void _or(uint32_t inst, state_t *s);
static void _sll(uint32_t inst, state_t *s);
static void _sllv(uint32_t inst, state_t *s);
static void _slt(uint32_t inst, state_t *s);
static void _sltu(uint32_t inst, state_t *s);
static void _sra(uint32_t inst, state_t *s);
static void _srav(uint32_t inst, state_t *s);
static void _srl(uint32_t inst, state_t *s);
static void _srlv(uint32_t inst, state_t *s);
static void _sub(uint32_t inst, state_t *s);
static void _subu(uint32_t inst, state_t *s);
static void _syscall(uint32_t inst, state_t *s);
static void _xor(uint32_t inst, state_t *s);
static void _tge(uint32_t inst, state_t *s);
static void _teq(uint32_t inst, state_t *s);

/* JType instructions */
static void _j(uint32_t inst, state_t *s);
static void _jal(uint32_t inst, state_t *s);

/* IType instructions */
static void _addi(uint32_t inst, state_t *s);
static void _addiu(uint32_t inst, state_t *s);
static void _andi(uint32_t inst, state_t *s);
static void _ori(uint32_t inst, state_t *s);
static void _xori(uint32_t inst, state_t *s);
static void _beq(uint32_t inst, state_t *s);
static void _beql(uint32_t inst, state_t *s);
static void _bne(uint32_t inst, state_t *s);
static void _bnel(uint32_t inst, state_t *s);

static void _bgtz(uint32_t inst, state_t *s);
static void _blez(uint32_t inst, state_t *s);

static void _bgez_bltz(uint32_t inst, state_t *s);

static void _lui(uint32_t inst, state_t *s);
static void _lw(uint32_t inst, state_t *s);
static void _lh(uint32_t inst, state_t *s);
static void _lb(uint32_t inst, state_t *s);
static void _lbu(uint32_t inst, state_t *s);
static void _lhu(uint32_t inst, state_t *s);
static void _ldc1(uint32_t inst, state_t *s);

static void _slti(uint32_t inst, state_t *s);
static void _sltiu(uint32_t inst, state_t *s);

static void _sw(uint32_t inst, state_t *s);
static void _sh(uint32_t inst, state_t *s);
static void _sb(uint32_t inst, state_t *s);
static void _sdc1(uint32_t inst, state_t *s);

static void _seh(uint32_t inst, state_t *s);
static void _ext(uint32_t inst, state_t *s);
static void _clz(uint32_t inst, state_t *s);

static void (*functTbl[64])(uint32_t inst, state_t *s) = {NULL};
static void (*ITypeOpcodeTbl[64])(uint32_t inst, state_t *s) = {NULL};

void printState(state_t *s)
{
  printf("executed %llu instructions, PC=0x%08x\n", s->icnt, s->pc);
  for(int32_t i = 0; i < 32; i++)
    {
      printf("%s: 0x%08x (%d)\n", getGPRName(i).c_str(), s->gpr[i], s->gpr[i]);  
    }
}

void initEmulationTables()
{
  /* These are R Type instructions (use function) */
  functTbl[0x00] = _sll;
  functTbl[0x02] = _srl;
  functTbl[0x03] = _sra;
  functTbl[0x04] = _sllv;
  /* Special instruction: based on gdb simulator */
  functTbl[0x05] = _monitor;
  functTbl[0x06] = _srlv;
  functTbl[0x07] = _srav;
  functTbl[0x08] = _jr;
  functTbl[0x09] = _jalr;
  functTbl[0x0C] = _syscall;
  functTbl[0x0D] = _break;
  functTbl[0x10] = _mfhi;
  functTbl[0x11] = _mthi;
  functTbl[0x12] = _mflo;
  functTbl[0x13] = _mtlo;
  functTbl[0x18] = _mult;
  functTbl[0x19] = _multu;
  functTbl[0x1A] = _div;
  functTbl[0x1B] = _divu;
  functTbl[0x20] = _add;
  functTbl[0x21] = _addu;
  functTbl[0x22] = _sub;
  functTbl[0x23] = _subu;
  functTbl[0x24] = _and;
  functTbl[0x25] = _or;
  functTbl[0x26] = _xor;
  functTbl[0x27] = _nor;
  functTbl[0x2A] = _slt;
  functTbl[0x2B] = _sltu;
  functTbl[0x0B] = _movn;
  functTbl[0x0A] = _movz;
  functTbl[0x34] = _teq;
  /* MIPS32 */
  functTbl[0x30] = _tge;
  /* These are I Type instructions (use opcode) */
  ITypeOpcodeTbl[0x08] = _addi;
  ITypeOpcodeTbl[0x09] = _addiu;
  ITypeOpcodeTbl[0x0c] = _andi;
  ITypeOpcodeTbl[0x0d] = _ori;
  ITypeOpcodeTbl[0x0e] = _xori;
  ITypeOpcodeTbl[0x04] = _beq;
  ITypeOpcodeTbl[0x14] = _beql;

  ITypeOpcodeTbl[0x05] = _bne;
  ITypeOpcodeTbl[0x15] = _bnel;
  
  ITypeOpcodeTbl[0x06] = _blez;
  ITypeOpcodeTbl[0x07] = _bgtz;

  ITypeOpcodeTbl[0x01] = _bgez_bltz;

  ITypeOpcodeTbl[0x0A] = _slti;
  ITypeOpcodeTbl[0x0B] = _sltiu;

  ITypeOpcodeTbl[0x0F] = _lui;
  ITypeOpcodeTbl[0x20] = _lb;
  ITypeOpcodeTbl[0x21] = _lh;
  ITypeOpcodeTbl[0x23] = _lw;

  ITypeOpcodeTbl[0x24] = _lbu;
  ITypeOpcodeTbl[0x25] = _lhu;

 
  ITypeOpcodeTbl[0x28] = _sb;
  ITypeOpcodeTbl[0x29] = _sh;
  ITypeOpcodeTbl[0x2B] = _sw;
  ITypeOpcodeTbl[0x3D] = _sdc1;
  ITypeOpcodeTbl[0x35] = _ldc1;
}

void execMips(state_t *s)
{
  /* We assume inst is little endian */
  uint8_t *mem = s->mem;
  uint32_t inst = accessBigEndian(*(uint32_t*)(mem + s->pc));

  
  //std::string asmString = getAsmString(inst, s->pc);
  //printf("%x: %s\n", s->pc, asmString.c_str());

  //std::string asmString = getAsmString(inst, s->pc);
  //log += "0x" + toStringHex(s->pc) + ": " + asmString + "\n";
  
  s->icnt++;

  uint32_t opcode = inst>>26;
  bool isRType = (opcode==0);
  bool isJType = ((opcode>>1)==1);
  bool isCoproc0 = (opcode == 0x10);
  bool isCoproc1 = (opcode == 0x11);
  bool isCoproc2 = (opcode == 0x12);
  bool isSpecial2 = (opcode == 0x1c); 
  bool isSpecial3 = (opcode == 0x1f);

  if(isRType)
    execRType(inst,s);
  else if(isSpecial2)
    execSpecial2(inst,s);
  else if(isSpecial3)
    execSpecial3(inst,s);
  else if(isJType)
    execJType(inst,s);
  else if(isCoproc0)
    execCoproc0(inst,s);
  else if(isCoproc1)
    execCoproc1(inst,s);
  else if(isCoproc2)
    execCoproc2(inst,s);
  else 
    execIType(inst,s);
}

void execSpecial2(uint32_t inst,state_t *s)
{
  uint32_t funct = inst & 63;
  switch(funct)
    {
    case(0x0):
      _madd(inst,s);
      break;
    case(0x2):
      _mul(inst,s);
      break;
    case(0x20):
      _clz(inst, s);
      break;
    default:
      printf("unhandled special2 instruction @ 0x%08x\n", s->pc); 
      exit(-1);
      break;
    }
  return;
}

void execSpecial3(uint32_t inst,state_t *s)
{
  uint32_t funct = inst & 63;
  uint32_t op = (inst>>6) & 31;
  if(funct == 32)
    {
      switch(op)
	{
	case 0x18:
	  _seh(inst, s);
	  break;
	default:
	  printf("unhandled special3 instruction @ 0x%08x\n", s->pc); 
	  exit(-1);    
	  break;
	}
    }
  /* EXT instruction */
  else if(funct == 0)
    {
      _ext(inst, s);
    }
  else
    {

    }
  //_seh;
  
  return;
}


void execRType(uint32_t inst,state_t *s)
{
  uint32_t funct = inst & 63;
  if(functTbl[funct] == 0)
    {
      printf("unknown RType instruction, funct = %d\n", funct);
      exit(-1);
    }
  else
    {
      functTbl[funct](inst, s);
    }
}

void execJType(uint32_t inst, state_t *s)
{
  uint32_t opcode = inst>>26;
  if(opcode==0x2)
    {
      _j(inst,s);
    }
  else if(opcode==0x3)
    {
      _jal(inst, s);
    }
  else
    {
      printf("Unknown JType instruction\n");
      exit(-1);
    }
}

void execIType(uint32_t inst, state_t *s)
{
  uint32_t opcode = inst>>26;
  if(ITypeOpcodeTbl[opcode] != 0)
    {
      ITypeOpcodeTbl[opcode](inst, s);
    }
  else
    {
      printf("Unknown IType instruction (opcode=%x) @ pc=0x%08x\n", opcode, s->pc);
      exit(-1);
    }
}

void execCoproc0(uint32_t inst, state_t *s)
{
  uint32_t opcode = inst>>26;
  uint32_t functField = (inst>>21) & 31;
  uint32_t sel = inst & 7;
  uint32_t rd = (inst>>11) & 31;
  uint32_t rt = (inst>>16) & 31;

  assert(sel==0);
  opcode &= 0x3;
  s->pc += 4;

  switch(functField)
    {
    case 0x0:
      /* move from coprocessor */
      s->gpr[rt] = s->cpr0[rd];
      break;
    case 0x4:
      /* move to coprocessor */
      s->cpr0[rd] = s->gpr[rt];
      break;
    default:
      printf("unknown %s instruction @ %x", __func__, s->pc);
      exit(-1);
      break;
    }
  
}
void execCoproc1(uint32_t inst, state_t *s)
{
  uint32_t opcode = inst>>26;
  uint32_t functField = (inst>>21) & 31;
  uint32_t lowop = inst & 63;
  uint32_t rd = (inst>>11) & 31;
  uint32_t rt = (inst>>16) & 31;

  uint32_t fd = (inst>>6) & 31;
  uint32_t fs = (inst>>11) & 31;

  uint32_t lowbits = inst & ((1<<11)-1);
  opcode &= 0x3;
  s->pc += 4;

  if(lowbits == 0)
    {
      switch(functField)
	{
	case 0x0:
	  /* move from coprocessor */
	  s->gpr[rt] = s->cpr1[rd];
	  break;
	case 0x4:
	  /* move to coprocessor */
	  s->cpr1[rd] = s->gpr[rt];
	  break;
	default:
	  printf("unhandled coproc1 instruction (functfield = %x) @ %08x\n", functField, s->pc);
	  exit(-1);
	  break;
	}
    }
  else
    {
      switch(lowop)
	{
	case 0x6:
	  /* MOV.D */
	  s->cpr1[fd] = s->cpr1[fs];
	  break;
	default:
	  printf("unhandled coproc1 instruction @ %08x\n", s->pc);
	  exit(-1);
	  break;
	}

    }
  
}

void execCoproc2(uint32_t inst, state_t *s)
{
  printf("%s\n", __func__);
  exit(-1);
}

/* RType instructions */
static void _teq(uint32_t inst, state_t *s)
{
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  if(s->gpr[rs] == s->gpr[rt])
    {
      printf("%s trap!!!!!\n", __func__);
      printState(s);
      exit(-1);
    }
  s->pc += 4;
}
static void _add(uint32_t inst, state_t *s)
{
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  s->gpr[rd] = s->gpr[rs] + s->gpr[rt];
  s->pc += 4;
}
static void _addu(uint32_t inst, state_t *s)
{
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;

  uint32_t u_rs = (uint32_t)s->gpr[rs];
  uint32_t u_rt = (uint32_t)s->gpr[rt];
  s->gpr[rd] = u_rs + u_rt;
  s->pc += 4;
}

static void _and(uint32_t inst, state_t *s)
{
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  s->gpr[rd] = s->gpr[rs] & s->gpr[rt];
  s->pc += 4;
}

static void _break(uint32_t inst, state_t *s)
{
  s->brk = 1;
}

static void _div(uint32_t inst, state_t *s)
{
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  if(s->gpr[rt] != 0)
    {
      s->lo = s->gpr[rs] / s->gpr[rt];
      s->hi = s->gpr[rs] % s->gpr[rt];
    }
  s->pc += 4;
}
static void _divu(uint32_t inst, state_t *s)
{ 
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  if(s->gpr[rt] != 0)
    {
      s->lo = (uint32_t)s->gpr[rs] / (uint32_t)s->gpr[rt];
      s->hi = (uint32_t)s->gpr[rs] % (uint32_t)s->gpr[rt];
    }
  s->pc += 4;
}
static void _jalr(uint32_t inst, state_t *s)
{
  uint32_t rs = (inst >> 21) & 31;
  uint32_t jaddr = s->gpr[rs];
  s->gpr[31] = s->pc+8;
  s->pc += 4;
  execMips(s);
  s->pc = jaddr;
}

static void _jr(uint32_t inst, state_t *s)
{
  uint32_t rs = (inst >> 21) & 31;
  uint32_t jaddr = s->gpr[rs];
  s->pc += 4;
  execMips(s);
  s->pc = jaddr;
}

static void _movn(uint32_t inst, state_t *s)
{
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  s->gpr[rd] = (s->gpr[rt] != 0) ? s->gpr[rs] : s->gpr[rd];
  s->pc +=4;
}
static void _movz(uint32_t inst, state_t *s)
{
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  s->gpr[rd] = (s->gpr[rt] == 0) ? s->gpr[rs] : s->gpr[rd];
  s->pc += 4;
}

static void _mfhi(uint32_t inst, state_t *s)
{
  uint32_t rd = (inst >> 11) & 31;
  s->gpr[rd] = s->hi;
  s->pc += 4;
}
static void _mflo(uint32_t inst, state_t *s)
{
  uint32_t rd = (inst >> 11) & 31;
  s->gpr[rd] = s->lo;
  s->pc += 4;
}
static void _mthi(uint32_t inst, state_t *s)
{
  uint32_t rs = (inst >> 21) & 31;
  s->gpr[rs] = s->hi;
  s->pc += 4;
}
static void _mtlo(uint32_t inst, state_t *s)
{
  uint32_t rs = (inst >> 21) & 31;
  s->gpr[rs] = s->lo;
  s->pc += 4;
}
static void _mult(uint32_t inst, state_t *s)
{
  int64_t y;
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  y = (int64_t)s->gpr[rs] * (int64_t)s->gpr[rt];
  s->lo = (int32_t)(y & 0xffffffff);
  s->hi = (int32_t)(y >> 32);
  s->pc += 4;
}

static void _madd(uint32_t inst, state_t *s)
{
  int64_t y,acc;
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  acc = ((int64_t)s->hi) << 32;
  acc |= ((int64_t)s->lo);
  y = (int64_t)s->gpr[rs] * (int64_t)s->gpr[rt];
  y += acc;
  s->lo = (int32_t)(y & 0xffffffff);
  s->hi = (int32_t)(y >> 32);
  s->pc += 4;
}

static void _multu(uint32_t inst, state_t *s)
{
  uint64_t y;
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  y = (uint64_t)s->gpr[rs] * (uint64_t)s->gpr[rt];
  s->lo = (int32_t)(y & 0xffffffff);
  s->hi = (int32_t)(y >> 32);
  s->pc += 4;
}

static void _mul(uint32_t inst, state_t *s)
{
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  s->gpr[rd] = s->gpr[rs] * s->gpr[rt];
  //printf("pc before = %x\n", s->pc);
  s->pc += 4;
  //printf("pc after = %x\n", s->pc);
  //exit(-1);
}

static void _nor(uint32_t inst, state_t *s)
{
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  s->gpr[rd] = ~(s->gpr[rs] | s->gpr[rt]);
  s->pc += 4;
}

static void _or(uint32_t inst, state_t *s)
{
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  s->gpr[rd] = s->gpr[rs] | s->gpr[rt];
  s->pc += 4;
}

static void _sll(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  uint32_t sa = (inst >> 6) & 31;
  s->gpr[rd] = s->gpr[rt] << sa;
  s->pc += 4;
}

static void _sllv(uint32_t inst, state_t *s)
{
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  
  s->gpr[rd] = s->gpr[rt] << s->gpr[rs];
  s->pc += 4;
}
static void _slt(uint32_t inst, state_t *s)
{
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  
  s->gpr[rd] = s->gpr[rs] < s->gpr[rt];
  s->pc += 4;
}

static void _sltu(uint32_t inst, state_t *s)
{
  /* s->gpr[rd] = (s->gpr[rs] < s->gpr[rt]) */
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  uint32_t urs = (uint32_t)s->gpr[rs];
  uint32_t urt = (uint32_t)s->gpr[rt];
  s->gpr[rd] = (urs < urt);
  s->pc += 4;
}

static void _sra(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  uint32_t sa = (inst >> 6) & 31;
  s->gpr[rd] = s->gpr[rt] >> sa;
  s->pc += 4;
}
static void _srav(uint32_t inst, state_t *s)
{
  printf("%s", __func__); 
  exit(-1);
}
static void _srl(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  uint32_t sa = (inst >> 6) & 31; 
  s->gpr[rd] = ((uint32_t)s->gpr[rt] >> sa);
  s->pc += 4;
}
static void _srlv(uint32_t inst, state_t *s)
{
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  s->gpr[rd] = ((uint32_t)s->gpr[rt] >> s->gpr[rs]);
  s->pc += 4;
}

static void _sub(uint32_t inst, state_t *s)
{
  printf("%s", __func__); 
  exit(-1);
}

static void _subu(uint32_t inst, state_t *s)
{
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;

  uint32_t u_rs = (uint32_t)s->gpr[rs];
  uint32_t u_rt = (uint32_t)s->gpr[rt];
  s->gpr[rd] = u_rs - u_rt;
  s->pc += 4;
}
static void _syscall(uint32_t inst, state_t *s)
{ 
  printf("%s", __func__); exit(-1); 
}
static void _xor(uint32_t inst, state_t *s)
{ 
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  s->gpr[rd] = s->gpr[rs] ^ s->gpr[rt];
  s->pc += 4;
}

static void _tge(uint32_t inst, state_t *s)
{
  printf("%s", __func__); 
  exit(-1); 
}

/* JType instructions */
static void _j(uint32_t inst, state_t *s)
{
  uint32_t jaddr = inst & ((1<<26)-1);
  jaddr <<= 2;
  s->pc += 4;
  jaddr |= (s->pc & (~((1<<28)-1)));
  execMips(s);
  s->pc = jaddr;
}
static void _jal(uint32_t inst, state_t *s)
{
  uint32_t jaddr = inst & ((1<<26)-1);
  jaddr <<= 2;
  s->gpr[31] = s->pc+8;
  s->pc += 4;
  jaddr |= (s->pc & (~((1<<28)-1)));
  execMips(s);
  s->pc = jaddr;
}

/* IType instructions */
static void _addi(uint32_t inst, state_t *s)
{
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  s->gpr[rt] = s->gpr[rs] + imm;  
  s->pc+=4;
}
static void _addiu(uint32_t inst, state_t *s)
{
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  s->gpr[rt] = s->gpr[rs] + imm;  
  s->pc+=4;
}

static void _andi(uint32_t inst, state_t *s)
{
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t imm = inst & ((1<<16) - 1);
  s->gpr[rt] = s->gpr[rs] & imm;
  s->pc += 4;
}
static void _ori(uint32_t inst, state_t *s)
{
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t imm = inst & ((1<<16) - 1);
  s->gpr[rt] = s->gpr[rs] | imm;
  s->pc += 4;
}
static void _xori(uint32_t inst, state_t *s)
{
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t imm = inst & ((1<<16) - 1);
  s->gpr[rt] = s->gpr[rs] ^ imm;
  s->pc += 4;
}
static void _beq(uint32_t inst, state_t *s)
{
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
    s->pc = (imm+npc);
  
}

static void _beql(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  bool takeBranch = (s->gpr[rt] == s->gpr[rs]);
  
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
  uint32_t npc = s->pc+4; 
  
  /* execute branch delay */
  if(takeBranch)
    {
      s->pc +=4;
      execMips(s);
      s->pc = (imm+npc);
    }
  else
    {
      s->pc += 8;
    }
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
  
  /* execute branch delay */
  if(takeBranch)
    {
      s->pc +=4;
      execMips(s);
      s->pc = (imm+npc);
    }
  else
    {
      s->pc += 8;
    }
}

static void _bgtz(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
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

static void _blez(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
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
  if(rt==0)
    {
      /* bltz : less than zero */
      takeBranch = (s->gpr[rs] < 0);
      s->pc += 4;
      execMips(s);
      if(takeBranch)
	s->pc = imm+npc;
      //s += "bltz " + regNames[rs] + "," + toStringHex(imm+npc);
    }
  else if(rt==1)
    {
      /* bgez : greater than or equal to zero */
      takeBranch = (s->gpr[rs] >= 0);
      s->pc += 4;
      execMips(s);
      if(takeBranch)
	s->pc = imm+npc;
      //s += "bgez " + regNames[rs] + "," + toStringHex(imm+npc);
    }
  else if(rt==2)
    {
      takeBranch = (s->gpr[rs] < 0);
      if(takeBranch)
	{
	  s->pc += 4;
	  execMips(s);
	  s->pc = imm+npc;
	}
      else
	{
	  s->pc += 8;
	}
    }
  else if(rt == 3)
    {
      /* greater than zero likely */
      takeBranch = (s->gpr[rs] >=0);
      if(takeBranch)
	{
	  s->pc += 4;
	  execMips(s);
	  s->pc = imm+npc;
	}
      else
	{
	  s->pc += 8;
	}
    }

}

static void _lui(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t imm = inst & ((1<<16) - 1);
  imm <<= 16;
  s->gpr[rt] = imm;
  s->pc += 4;
}
static void _lw(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  /* mem[s->gpr[rs] + imm] = s->gpr[rt] */

  uint32_t ea = (uint32_t)s->gpr[rs] + imm;

  s->gpr[rt] = accessBigEndian(*((int32_t*)(s->mem + ea))); 
  /* printf("lw (pc=0x%x): loading from address 0x%x, value = %x\n",
     s->pc, ea, s->gpr[rt]); */
  s->pc += 4;
}

static void _ldc1(uint32_t inst, state_t *s)
{
  uint32_t ft = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  /* mem[s->gpr[rs] + imm] = s->gpr[rt] */
  uint32_t ea = s->gpr[rs] + imm;
  s->cpr1[ft] = accessBigEndian(*((int64_t*)(s->mem + ea))); 
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

static void _lb(uint32_t inst, state_t *s){}
static void _lbu(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  
  uint32_t ea = s->gpr[rs] + imm;
  s->gpr[rt] = (int32_t)s->mem[ea];
  s->pc += 4;
}

static void _lhu(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  
  uint32_t ea = s->gpr[rs] + imm;
  s->gpr[rt] = (int32_t)accessBigEndian(*((int16_t*)(s->mem + ea)));
  s->pc += 4;
}

static void _slti(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  s->gpr[rt] = (s->gpr[rs] < imm);
  s->pc += 4;
}

static void _sltiu(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  s->gpr[rt] = ((uint32_t)s->gpr[rs] < (uint32_t)imm);
  s->pc += 4;
}

static void _sw(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  /* mem[s->gpr[rs] + imm] = s->gpr[rt] */
  
  uint32_t ea = s->gpr[rs] + imm;
  *((int32_t*)(s->mem + ea)) = accessBigEndian(s->gpr[rt]);
  s->pc += 4;
}

static void _sdc1(uint32_t inst, state_t *s)
{
  uint32_t ft = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  /* mem[s->gpr[rs] + imm] = s->gpr[rt] */
  uint32_t ea = s->gpr[rs] + imm;
  *((int64_t*)(s->mem + ea)) = accessBigEndian(s->cpr1[ft]);
  //*((int32_t*)(s->mem + ea)) = accessBigEndian(s->gpr[rt]);
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

static void _seh(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  s->gpr[rd] = (int32_t)((int16_t)s->gpr[rt]);
  s->pc +=4;
}

static void _ext(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  uint32_t pos = (inst >> 6) & 31;
  uint32_t size = ((inst >> 11) & 31) + 1;
  /* store in rt */
  s->gpr[rt] = (s->gpr[rs] >> pos) & ((1<<size)-1);
  s->pc += 4;
}

static void _clz(uint32_t inst, state_t *s)
{
  uint32_t rd = (inst >> 11) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  
  /* store to rd */
  s->gpr[rd] = (s->gpr[rs]==0) ? 32 : __builtin_clz(s->gpr[rs]);
  s->pc += 4;
}

void initState(state_t *s)
{
  memset(s, 0, sizeof(state_t));
  /* setup the status register */
  s->cpr0[12] |= 1<<2;
  s->cpr0[12] |= 1<<22;
}

void mkMonitorVectors(state_t *s)
{
  for (uint32_t loop = 0; (loop < IDT_MONITOR_SIZE); loop += 4)
    {
      uint32_t vaddr = IDT_MONITOR_BASE + loop;
      uint32_t insn = (RSVD_INSTRUCTION |
			 (((loop >> 2) & RSVD_INSTRUCTION_ARG_MASK)
			  << RSVD_INSTRUCTION_ARG_SHIFT));
      /* printf("reserved isns = %x\n", insn); */
      *(uint32_t*)(s->mem+vaddr) = accessBigEndian(insn);
    }
}

static void _monitor(uint32_t inst, state_t *s)
{
  uint32_t reason = (inst >> RSVD_INSTRUCTION_ARG_SHIFT) & 
    RSVD_INSTRUCTION_ARG_MASK;
  reason >>= 1;
  int32_t fd=-1,nr=-1;
  switch(reason)
    {
    case 6: /* int open(char *path, int flags) */
      s->gpr[R_v0] = open((char*)(s->mem + (uint32_t)s->gpr[R_a0]), s->gpr[R_a1]);
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
      /* printf("fd = %d, nr = %d\n", fd, nr); */
      s->gpr[R_v0] = (int32_t)write(fd, (void*)(s->mem + (uint32_t)s->gpr[R_a1]), nr);
      if(fd==1)
	fflush(stdout);
      else if(fd==2)
	fflush(stderr);
      //printf("%s", (char*)(s->mem + (uint32_t)s->gpr[R_a1]));
      break;
    case 10:
      fd = s->gpr[R_a0];
      if(fd>2)
	s->gpr[R_v0] = (int32_t)close(fd);
      else
	s->gpr[R_v0] = 0;
      break;
    case 55: 
      /* void get_mem_info(unsigned int *ptr) */
      /* in:  A0 = pointer to three word memory location */
      /* out: [A0 + 0] = size */
      /*      [A0 + 4] = instruction cache size */
      /*      [A0 + 8] = data cache size */
      /* 256 MBytes of DRAM */
      *((uint32_t*)(s->mem + (uint32_t)s->gpr[R_a0] + 0)) = 
      	accessBigEndian(K1SIZE);
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
