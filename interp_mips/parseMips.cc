#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <list>

#include "helper.h"
#include "emulateMips.h"

static const std::string regNames[32] = 
  {
    "zero","at", "v0", "v1",
    "a0", "a1", "a2", "a3",
    "t0", "t1", "t2", "t3",
    "t4", "t5", "t6", "t7",
    "s0", "s1", "s2", "s3",
    "s4", "s5", "s6", "s7",
    "t8", "t9", "k0", "k1",
    "gp", "sp", "s8", "ra"
  };

std::string getGPRName(uint32_t r)
{
  r = r&31;
  return (r==0) ? regNames[r] : "  " + regNames[r];
}

static std::string decodeRType(uint32_t inst, uint32_t addr);
static std::string decodeJType(uint32_t inst, uint32_t addr);
static std::string decodeIType(uint32_t inst, uint32_t addr);
static std::string decodeSpecial2(uint32_t inst, uint32_t addr);
static std::string decodeSpecial3(uint32_t inst, uint32_t addr);

static std::string decodeCoproc0(uint32_t inst, uint32_t addr);
static std::string decodeCoproc1(uint32_t inst, uint32_t addr);
static std::string decodeCoproc2(uint32_t inst, uint32_t addr);

/* RType instructions */
static void _monitor(uint32_t inst, uint32_t addr, std::string &s);

static void _add(uint32_t inst, uint32_t addr, std::string &s);
static void _addu(uint32_t inst, uint32_t addr, std::string &s);
static void _and(uint32_t inst, uint32_t addr, std::string &s);
static void _break(uint32_t inst, uint32_t addr, std::string &s);
static void _div(uint32_t inst, uint32_t addr, std::string &s);
static void _divu(uint32_t inst, uint32_t addr, std::string &s);
static void _jalr(uint32_t inst, uint32_t addr, std::string &s);
static void _jr(uint32_t inst, uint32_t addr, std::string &s);
static void _movn(uint32_t inst, uint32_t addr, std::string &s);
static void _movz(uint32_t inst, uint32_t addr, std::string &s);

static void _mfhi(uint32_t inst, uint32_t addr, std::string &s);
static void _mflo(uint32_t inst, uint32_t addr, std::string &s);
static void _mthi(uint32_t inst, uint32_t addr, std::string &s);
static void _mtlo(uint32_t inst, uint32_t addr, std::string &s);
static void _mult(uint32_t inst, uint32_t addr, std::string &s);
static void _multu(uint32_t inst, uint32_t addr, std::string &s);
static void _madd(uint32_t inst, uint32_t addr, std::string &s);
static void _mul(uint32_t inst, uint32_t addr, std::string &s);

static void _nor(uint32_t inst, uint32_t addr, std::string &s);
static void _or(uint32_t inst, uint32_t addr, std::string &s);
static void _sll(uint32_t inst, uint32_t addr, std::string &s);
static void _sllv(uint32_t inst, uint32_t addr, std::string &s);
static void _slt(uint32_t inst, uint32_t addr, std::string &s);
static void _sltu(uint32_t inst, uint32_t addr, std::string &s);
static void _sra(uint32_t inst, uint32_t addr, std::string &s);
static void _srav(uint32_t inst, uint32_t addr, std::string &s);
static void _srl(uint32_t inst, uint32_t addr, std::string &s);
static void _srlv(uint32_t inst, uint32_t addr, std::string &s);
static void _sub(uint32_t inst, uint32_t addr, std::string &s);
static void _subu(uint32_t inst, uint32_t addr, std::string &s);
static void _syscall(uint32_t inst, uint32_t addr, std::string &s);
static void _xor(uint32_t inst, uint32_t addr, std::string &s);
static void _tge(uint32_t inst, uint32_t addr, std::string &s);
static void _teq(uint32_t inst, uint32_t addr, std::string &s);

/* JType instructions */
static void _j(uint32_t inst, uint32_t addr, std::string &s);
static void _jal(uint32_t inst, uint32_t addr, std::string &s);

/* IType instructions */
static void _addi(uint32_t inst, uint32_t addr, std::string &s);
static void _addiu(uint32_t inst, uint32_t addr, std::string &s);
static void _andi(uint32_t inst, uint32_t addr, std::string &s);
static void _ori(uint32_t inst, uint32_t addr, std::string &s);
static void _xori(uint32_t inst, uint32_t addr, std::string &s);
static void _beq(uint32_t inst, uint32_t addr, std::string &s);
static void _beql(uint32_t inst, uint32_t addr, std::string &s);
static void _bne(uint32_t inst, uint32_t addr, std::string &s);
static void _bnel(uint32_t inst, uint32_t addr, std::string &s);
static void _bgtzl(uint32_t inst, uint32_t addr, std::string &s);
static void _bgtz(uint32_t inst, uint32_t addr, std::string &s);
static void _blez(uint32_t inst, uint32_t addr, std::string &s);
static void _blezl(uint32_t inst, uint32_t addr, std::string &s);

static void _bgez_bltz(uint32_t inst, uint32_t addr, std::string &s);

static void _lui(uint32_t inst, uint32_t addr, std::string &s);
static void _lw(uint32_t inst, uint32_t addr, std::string &s);
static void _lh(uint32_t inst, uint32_t addr, std::string &s);
static void _lb(uint32_t inst, uint32_t addr, std::string &s);
static void _lbu(uint32_t inst, uint32_t addr, std::string &s);
static void _lhu(uint32_t inst, uint32_t addr, std::string &s);


static void _slti(uint32_t inst, uint32_t addr, std::string &s);
static void _sltiu(uint32_t inst, uint32_t addr, std::string &s);

static void _sw(uint32_t inst, uint32_t addr, std::string &s);
static void _sh(uint32_t inst, uint32_t addr, std::string &s);
static void _sb(uint32_t inst, uint32_t addr, std::string &s);

static void _sdc1(uint32_t inst, uint32_t addr, std::string &s);
static void _ldc1(uint32_t inst, uint32_t addr, std::string &s);

static void _ext(uint32_t inst, uint32_t addr, std::string &s);
static void _seh(uint32_t inst, uint32_t addr, std::string &s);
static void _clz(uint32_t inst, uint32_t addr, std::string &s);

static void _swl(uint32_t inst, uint32_t addr, std::string &s);
static void _swr(uint32_t inst, uint32_t addr, std::string &s);
static void _lwl(uint32_t inst, uint32_t addr, std::string &s);
static void _lwr(uint32_t inst, uint32_t addr, std::string &s);



static void (*functTbl[64])(uint32_t inst, uint32_t addr, std::string &s) = {NULL};
static void (*ITypeOpcodeTbl[64])(uint32_t inst, uint32_t addr, std::string &s) = {NULL};

void initParseTables()
{
  /* These are R Type instructions (use function) */
  functTbl[0x00] = _sll;
  functTbl[0x02] = _srl;
  functTbl[0x03] = _sra;
  functTbl[0x04] = _sllv;
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
  /* MIPS32 */
  functTbl[0x30] = _tge;
  functTbl[0x34] = _teq;

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
  ITypeOpcodeTbl[0x17] = _bgtzl;

  ITypeOpcodeTbl[0x06] = _blez;
  ITypeOpcodeTbl[0x16] = _blezl;
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

  ITypeOpcodeTbl[0x2a] = _swl;
  ITypeOpcodeTbl[0x2e] = _swr;
  ITypeOpcodeTbl[0x22] = _lwl;
  ITypeOpcodeTbl[0x26] = _lwr;
}

std::string getAsmString(uint32_t inst, uint32_t addr)
{
  /* We assume inst is little endian */
  uint32_t opcode = inst>>26;
  bool isRType = (opcode==0);
  bool isJType = ((opcode>>1)==1);
  bool isCoproc0 = (opcode == 0x10);
  bool isCoproc1 = (opcode == 0x11);
  bool isCoproc2 = (opcode == 0x12);
  bool isSpecial2 = (opcode == 0x1c); 
  bool isSpecial3 = (opcode == 0x1f);

  if(isRType)
    return decodeRType(inst,addr);
  else if(isSpecial2)
    return decodeSpecial2(inst,addr);
  else if(isSpecial3)
    return decodeSpecial3(inst,addr);
  else if(isJType)
    return decodeJType(inst,addr);
  else if(isCoproc0)
    return decodeCoproc0(inst,addr);
  else if(isCoproc1)
    return decodeCoproc1(inst,addr);
  else if(isCoproc2)
    return decodeCoproc2(inst,addr);
  else 
    return decodeIType(inst,addr);
}

static std::string decodeSpecial2(uint32_t inst,uint32_t addr)
{
  std::string s;
  uint32_t funct = inst & 63;
  switch(funct)
    {
    case(0x0):
      _madd(inst, addr, s);
      break;
    case(0x2):
      _mul(inst, addr, s);
      break;
    case(0x20):
      _clz(inst, addr, s);
      break;
    default:
      s = "unknown special2 instruction";
      break;
    }
  return s;
}

static std::string decodeSpecial3(uint32_t inst,uint32_t addr)
{
  std::string s;
  uint32_t funct = inst & 63;
  uint32_t op = (inst>>6) & 31;
  if(funct == 32)
    {
      switch(op)
	{
	case 0x18:
	  _seh(inst, addr, s);
	  break;
	default:
	  printf("unhandled special3 instruction @ 0x%08x\n", addr); 
	  exit(-1);    
	  break;
	}
    }
  /* EXT instruction */
  else if(funct == 0)
    {
      _ext(inst, addr, s);
    }
  else
    {

    }
  //_seh;
  return s;
}


static std::string decodeRType(uint32_t inst,uint32_t addr)
{
  std::string s;
  uint32_t funct = inst & 63;
  if(functTbl[funct] == 0)
    {
      s += "unknown RType instruction";
    }
  else
    {
      functTbl[funct](inst, addr, s);
    }
  return s;
}

static std::string decodeJType(uint32_t inst,uint32_t addr)
{
  std::string s;
  uint32_t opcode = inst>>26;
  if(opcode==0x2)
    {
      _j(inst, addr, s);
    }
  else if(opcode==0x3)
    {
      _jal(inst, addr, s);
    }
  else
    {
      s += "unknown JType instruction";
    }
  return s;
}

static std::string decodeIType(uint32_t inst,uint32_t addr)
{
  uint32_t opcode = inst>>26;
  std::string s;
  if(ITypeOpcodeTbl[opcode] != 0)
    {
      ITypeOpcodeTbl[opcode](inst, addr, s);
    }
  else
    {
      s += "unknown IType instruction, inst = 0x" + toStringHex(opcode) + 
	"(addr = " + toStringHex(addr) + ")";
      //printf("ERROR = %s\n", s.c_str());
      //exit(-1);
    }
  return s;
}

static std::string decodeCoproc0(uint32_t inst,uint32_t addr)
{
  std::string s;
  uint32_t opcode = inst>>26;
  uint32_t functField = (inst>>21) & 31;
  uint32_t sel = inst & 7;
  uint32_t rd = (inst>>11) & 31;
  uint32_t rt = (inst>>16) & 31;
  opcode &= 0x3;
  switch(functField)
    {
    case 0x0:
      /* move from coprocessor */
      s += "mfc" + toString(opcode) + " " + regNames[rt] + "," + toString(rd);
      break;
    case 0x4:
      /* move to coprocessor */
      s += "mtc" + toString(opcode) + " " + regNames[rt] + "," + toString(rd);
      break;
    case 0x6:
      /* floating-point move, type in sel field */
      break;
    default:
      s += std::string(__func__);
      printf("unknown %s instruction (field=%d) @ %08x\n", __func__, functField, addr);
      exit(-1);
      break;
    }
  return s;
}
static std::string decodeCoproc1(uint32_t inst,uint32_t addr)
{
  std::string s;
  uint32_t opcode = inst>>26;
  uint32_t functField = (inst>>21) & 31;
  uint32_t sel = inst & 7;
  uint32_t rd = (inst>>11) & 31;
  uint32_t rt = (inst>>16) & 31;
  opcode &= 0x3;
  switch(functField)
    {
    case 0x0:
      /* move from coprocessor */
      s += "mfc" + toString(opcode) + " " + regNames[rt] + "," + toString(rd);
      break;
    case 0x4:
      /* move to coprocessor */
      s += "mtc" + toString(opcode) + " " + regNames[rt] + "," + toString(rd);
      break;
    case 0x6:
      /* floating-point move, type in sel field */
      break;
    default:
      s += std::string(__func__);
      printf("unknown %s instruction (field=%d) @ %08x\n", __func__, functField, addr);
      break;
    }
  return s;
}
static std::string decodeCoproc2(uint32_t inst,uint32_t addr)
{
  std::string s;
  uint32_t opcode = inst>>26;
  uint32_t functField = (inst>>21) & 31;
  uint32_t sel = inst & 7;
  uint32_t rd = (inst>>11) & 31;
  uint32_t rt = (inst>>16) & 31;
  opcode &= 0x3;
  switch(functField)
    {
    case 0x0:
      /* move from coprocessor */
      s += "mfc" + toString(opcode) + " " + regNames[rt] + "," + toString(rd);
      break;
    case 0x4:
      /* move to coprocessor */
      s += "mtc" + toString(opcode) + " " + regNames[rt] + "," + toString(rd);
      break;
    case 0x6:
      /* floating-point move, type in sel field */
      break;
    default:
      s += std::string(__func__);
      printf("unknown %s instruction (field=%d) @ %08x\n", __func__, functField, addr);
      break;
    }
  return s;
}

/* JType instructions */
static void _j(uint32_t inst, uint32_t addr, std::string &s)
{
  uint32_t jaddr = inst & ((1<<26)-1);
  jaddr <<= 2;
  jaddr |= ((addr + 4) & (~((1<<28)-1)));
  s += "j " + toStringHex(jaddr);
}

static void _jal(uint32_t inst, uint32_t addr, std::string &s)
{
  uint32_t jaddr = inst & ((1<<26)-1);
  jaddr <<= 2;
  jaddr |= ((addr + 4) & (~((1<<28)-1)));
  s += "jal " + toStringHex(jaddr);
}

static void _add(uint32_t inst, uint32_t addr, std::string &s)
{
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  s += "add " + regNames[rd] + "," + regNames[rs] + "," + regNames[rt];
}

static void _addu(uint32_t inst, uint32_t addr, std::string &s)
{
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  s += "addu " + regNames[rd] + "," + regNames[rs] + "," + regNames[rt];
}

static void _and(uint32_t inst, uint32_t addr, std::string &s)
{
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  s += "and " + regNames[rd] + "," + regNames[rs] + "," + regNames[rt];
}

static void _break(uint32_t inst, uint32_t addr, std::string &s)
{
  s += std::string(__func__);
}

static void _div(uint32_t inst, uint32_t addr, std::string &s)
{
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  s += "div " + regNames[rs]  + "," + regNames[rt];
}

static void _divu(uint32_t inst, uint32_t addr, std::string &s)
{
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  s += "divu " + regNames[rs]  + "," + regNames[rt];
}

static void _jalr(uint32_t inst, uint32_t addr, std::string &s)
{
  uint32_t rs = (inst >> 21) & 31;
  s += "jalr " + regNames[rs];
}

static void _jr(uint32_t inst, uint32_t addr, std::string &s)
{
  uint32_t rs = (inst >> 21) & 31;
  s += "jr " + regNames[rs];
}

static void _mfhi(uint32_t inst, uint32_t addr, std::string &s)
{
  uint32_t rd = (inst >> 11) & 31;
  s += "mfhi " + regNames[rd];
}
static void _mflo(uint32_t inst, uint32_t addr, std::string &s)
{
  uint32_t rd = (inst >> 11) & 31;
  s += "mflo " + regNames[rd];
}

static void _mult(uint32_t inst, uint32_t addr, std::string &s)
{
  s += std::string(__func__);
}

static void _mul(uint32_t inst, uint32_t addr, std::string &s)
{
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  s += "mul " + regNames[rd] + "," + regNames[rs] + "," + regNames[rt];
}

static void _madd(uint32_t inst, uint32_t addr, std::string &s)
{
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  s += "madd " + regNames[rs] + "," + regNames[rt];
}

static void _multu(uint32_t inst, uint32_t addr, std::string &s)
{
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  s += "multu " + regNames[rs] + "," + regNames[rt];
}

static void _nor(uint32_t inst, uint32_t addr, std::string &s)
{
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  s += "nor " + regNames[rd] + "," + regNames[rs] + "," + regNames[rt];
}

static void _or(uint32_t inst, uint32_t addr, std::string &s)
{
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  s += "or " + regNames[rd] + "," + regNames[rs] + "," + regNames[rt];
}

static void _sll(uint32_t inst, uint32_t addr, std::string &s)
{
  if(inst==0)
    {
      s += "nop";
    }
  else
    {
      uint32_t rt = (inst >> 16) & 31;
      uint32_t rd = (inst >> 11) & 31;
      uint32_t sa = (inst >> 6) & 31;

      s += "sll " + regNames[rd] + "," + 
	regNames[rt] + ",0x" + toStringHex(sa);
    }
}

static void _sllv(uint32_t inst, uint32_t addr, std::string &s)
{
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;

  s += "sllv " + regNames[rd] + "," + regNames[rt] + "," + regNames[rs];
}
static void _slt(uint32_t inst, uint32_t addr, std::string &s)
{
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  s += "slt " + regNames[rd] + "," + regNames[rs] + "," + regNames[rt];
}
static void _sltu(uint32_t inst, uint32_t addr, std::string &s)
{
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  s += "sltu " + regNames[rd] + "," + regNames[rs] + "," + regNames[rt];
}

static void _sra(uint32_t inst, uint32_t addr, std::string &s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  uint32_t sa = (inst >> 6) & 31;

  s += "sra " + regNames[rd] + "," + 
    regNames[rt] + ",0x" + toStringHex(sa);
}
static void _srav(uint32_t inst, uint32_t addr, std::string &s)
{
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;

  s += "srav " + regNames[rd] + "," + regNames[rt] + "," + regNames[rs];
}
static void _srl(uint32_t inst, uint32_t addr, std::string &s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  uint32_t sa = (inst >> 6) & 31;

  s += "srl " + regNames[rd] + "," + 
    regNames[rt] + ",0x" + toStringHex(sa);
}
static void _srlv(uint32_t inst, uint32_t addr, std::string &s)
{
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;

  s += "srlv " + regNames[rd] + "," + regNames[rt] + "," + regNames[rs];
}
static void _sub(uint32_t inst, uint32_t addr, std::string &s)
{
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  s += "sub " + regNames[rd] + "," + regNames[rs] + "," + regNames[rt];
}

static void _subu(uint32_t inst, uint32_t addr, std::string &s)
{
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  s += "subu " + regNames[rd] + "," + regNames[rs] + "," + regNames[rt];
}

static void _syscall(uint32_t inst, uint32_t addr, std::string &s)
{ 
  s += std::string(__func__);
}

static void _xor(uint32_t inst, uint32_t addr, std::string &s)
{
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  s += "xor " + regNames[rd] + "," + regNames[rs] + "," + regNames[rt];
}

static void _movn(uint32_t inst, uint32_t addr, std::string &s)
{
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  /* gpr[rd]  = gpr[rt] != 0 ? gpr[rs] : gpr[rd]; */
  s += "movn " + regNames[rd] + "," + regNames[rs] + "," + regNames[rt];
}

static void _movz(uint32_t inst, uint32_t addr, std::string &s)
{
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  /* gpr[rd]  = gpr[rt] == 0 ? gpr[rs] : gpr[rd]; */
  s += "movz " + regNames[rd] + "," + regNames[rs] + "," + regNames[rt];
}

static void _mthi(uint32_t inst, uint32_t addr, std::string &s)
{
  s += std::string(__func__);
}

static void _mtlo(uint32_t inst, uint32_t addr, std::string &s)
{
  s += std::string(__func__);
}

static void _tge(uint32_t inst, uint32_t addr, std::string &s)
{
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  s += std::string(__func__);
}


static void _teq(uint32_t inst, uint32_t addr, std::string &s)
{
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  s += "teq " + regNames[rs] + "," + regNames[rt];
}

static void _addi(uint32_t inst, uint32_t addr, std::string &s)
{
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  
  s += "addi " + regNames[rt] + "," + regNames[rs] + "," + 
    toString<int32_t>(imm);
}

static void _addiu(uint32_t inst, uint32_t addr, std::string &s)
{
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t imm = inst & ((1<<16) - 1);
  s += "addiu " + regNames[rt] + "," + regNames[rs] + "," + 
    toString<uint32_t>(imm);
}

static void _andi(uint32_t inst, uint32_t addr, std::string &s)
{
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t imm = inst & ((1<<16) - 1);
  s += "andi " + regNames[rt] + "," + regNames[rs] + "," + 
    toString<uint32_t>(imm);
}

static void _ori(uint32_t inst, uint32_t addr, std::string &s)
{
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t imm = inst & ((1<<16) - 1);
  s += "ori " + regNames[rt] + "," + regNames[rs] + "," + 
    toString<uint32_t>(imm);
}

static void _xori(uint32_t inst, uint32_t addr, std::string &s)
{
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t imm = inst & ((1<<16) - 1);
  s += "xori " + regNames[rt] + "," + regNames[rs] + "," + 
    toString<uint32_t>(imm);
}

static void _beq(uint32_t inst, uint32_t addr,std::string &s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
  int32_t npc = addr+4; 
  s += "beq " + regNames[rs] + "," + regNames[rt] + "," +
    toStringHex(imm+npc);
}

static void _beql(uint32_t inst, uint32_t addr,std::string &s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
  int32_t npc = addr+4; 
  char buf[80] = {0};
  s += "beql " + regNames[rs] + "," + regNames[rt] + "," +
    toStringHex((uint32_t)(imm+npc));
}

static void _bgez_bltz(uint32_t inst, uint32_t addr,std::string &s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
  int32_t npc = addr+4; 
  if(rt==0)
    {
      /* bltz : less than zero */
      s += "bltz " + regNames[rs] + "," + toStringHex(imm+npc);
    }
  else if(rt == 1)
    {
      /* bgez : greater than or equal to zero */
      s += "bgez " + regNames[rs] + "," + toStringHex(imm+npc);
    }
  else if(rt == 2)
    {
      /* bltz : less than zero likely */
      s += "bltzl " + regNames[rs] + "," + toStringHex(imm+npc);
    }
  else if(rt == 3)
    {
      s += "bgezl " + regNames[rs] + "," + toStringHex(imm+npc);
    }
  else
    {
      printf("unknown branch type, rt=%d, addr = %x!\n", (int32_t)rt, addr);
      exit(-1);
    }

}

static void _bne(uint32_t inst, uint32_t addr,std::string &s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
  int32_t npc = addr+4; 
  s += "bne " + regNames[rs] + "," + regNames[rt] + "," +
    toStringHex(imm+npc);
}

static void _bnel(uint32_t inst, uint32_t addr,std::string &s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
  int32_t npc = addr+4; 
  s += "bnel " + regNames[rs] + "," + regNames[rt] + "," +
    toStringHex(imm+npc);
}

static void _bgtz(uint32_t inst, uint32_t addr, std::string &s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
  int32_t npc = addr+4; 
  assert(rt == 0);
  s += "bgtz " + regNames[rs] + "," + toStringHex(imm+npc);
}

static void _bgtzl(uint32_t inst, uint32_t addr, std::string &s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
  int32_t npc = addr+4; 
  assert(rt == 0);
  s += "bgtzl " + regNames[rs] + "," + toStringHex(imm+npc);
}

static void _blez(uint32_t inst, uint32_t addr, std::string &s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
  int32_t npc = addr+4; 
  assert(rt == 0);
  s += "blez " + regNames[rs] + "," + toStringHex(imm+npc);
}

static void _blezl(uint32_t inst, uint32_t addr, std::string &s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
  int32_t npc = addr+4; 
  assert(rt == 0);
  s += "blezl " + regNames[rs] + "," + toStringHex(imm+npc);
}

static void _lui(uint32_t inst, uint32_t addr,std::string &s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t imm = inst & ((1<<16) - 1);
  imm <<= 16;
  s += "lui " + regNames[rt] + ",0x" + toStringHex(imm);
}

static void _lw(uint32_t inst, uint32_t addr,std::string &s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  
  s += "lw " + regNames[rt] + "," +  toString<int32_t>(imm) +
    "(" + regNames[rs] + ")";
}

static void _ldc1(uint32_t inst, uint32_t addr,std::string &s)
{
  uint32_t ft = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  
  s += "ldc1 f" + toString(ft) + "," +  toString<int32_t>(imm) +
    "(" + regNames[rs] + ")";
}

static void _lh(uint32_t inst, uint32_t addr,std::string &s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  
  s += "lh " + regNames[rt] + "," +  toString<int32_t>(imm) +
    "(" + regNames[rs] + ")";
}

static void _lhu(uint32_t inst, uint32_t addr,std::string &s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  
  s += "lhu " + regNames[rt] + "," +  toString<int32_t>(imm) +
    "(" + regNames[rs] + ")";
}


static void _lbu(uint32_t inst, uint32_t addr,std::string &s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  
  s += "lbu " + regNames[rt] + "," +  toString<int32_t>(imm) +
    "(" + regNames[rs] + ")";
}



static void _lb(uint32_t inst, uint32_t addr,std::string &s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  
  s += "lb " + regNames[rt] + "," +  toString<int32_t>(imm) +
    "(" + regNames[rs] + ")";
}


static void _sw(uint32_t inst, uint32_t addr,std::string &s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  
  s += "sw " + regNames[rt] + "," +  toString<int32_t>(imm) +
    "(" + regNames[rs] + ")";
}

static void _sh(uint32_t inst, uint32_t addr,std::string &s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  
  s += "sh " + regNames[rt] + "," +  toString<int32_t>(imm) +
    "(" + regNames[rs] + ")";
}

static void _sb(uint32_t inst, uint32_t addr,std::string &s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  
  s += "sb " + regNames[rt] + "," +  toString<int32_t>(imm) +
    "(" + regNames[rs] + ")";
}

static void _slti(uint32_t inst, uint32_t addr,std::string &s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  
  s += "slti " + regNames[rt] + "," + regNames[rs] + "," +  
    toString<int32_t>(imm);
}

static void _sltiu(uint32_t inst, uint32_t addr,std::string &s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  uint16_t imm = (inst & ((1<<16) - 1));
    
  s += "sltiu " + regNames[rt] + "," + regNames[rs] + "," +  
    toString(imm);
}

static void _sdc1(uint32_t inst, uint32_t addr,std::string &s)
{
  uint32_t ft = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  
  s += "sdc1 f" + toString(ft) + "," +  toString<int32_t>(imm) +
    "(" + regNames[rs] + ")";
}


static void _monitor(uint32_t inst, uint32_t addr,std::string &s)
{
  uint32_t reason = (inst >> RSVD_INSTRUCTION_ARG_SHIFT) & RSVD_INSTRUCTION_ARG_MASK;
  reason >>= 1;
  s += "rsvd (monitor " + toString(reason) + ")";
}

static void _ext(uint32_t inst, uint32_t addr,std::string &s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  uint32_t pos = (inst >> 6) & 31;
  uint32_t size = ((inst >> 11) & 31) + 1;
  s += "ext " + regNames[rt] + "," + regNames[rs] + ",0x" + toStringHex(pos) + ",0x" + toStringHex(size);
  /* store in rt */
}

static void _seh(uint32_t inst, uint32_t addr,std::string &s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  s += "ext " + regNames[rd] + "," + regNames[rt];
}

static void _clz(uint32_t inst, uint32_t addr,std::string &s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  s += "ext " + regNames[rd] + "," + regNames[rt];
}

static void _swl(uint32_t inst, uint32_t addr,std::string &s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  
  s += "swl " + regNames[rt] + "," +  toString<int32_t>(imm) +
    "(" + regNames[rs] + ")";
}
static void _swr(uint32_t inst, uint32_t addr,std::string &s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  
  s += "swr " + regNames[rt] + "," +  toString<int32_t>(imm) +
    "(" + regNames[rs] + ")";
}
static void _lwl(uint32_t inst, uint32_t addr,std::string &s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  
  s += "lwl " + regNames[rt] + "," +  toString<int32_t>(imm) +
    "(" + regNames[rs] + ")";
}
static void _lwr(uint32_t inst, uint32_t addr,std::string &s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  
  s += "lwr " + regNames[rt] + "," +  toString<int32_t>(imm) +
    "(" + regNames[rs] + ")";
}
