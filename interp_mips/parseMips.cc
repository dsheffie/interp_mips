#include "helper.hh"
#include "profileMips.hh"
#include "parseMips.hh"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <list>

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
static const std::string condNames[16] =
  {
    "f", "un", "eq", "ueq",
    "olt", "ult", "ole", "ule",
    "sf", "ngle", "seq", "ngl",
    "lt", "nge", "le", "ngt"
  };

static const std::string instNames[] = 
  {
    "monitor", 
    "add", 
    "addu", 
    "and",
    "break",
    "div",
    "divu",
    "jalr",
    "jr",
    "movn",
    "movz",
    "mfhi",
    "mflo",
    "mthi",
    "mtlo",
    "mult",
    "multu",
    "madd",
    "maddu",
    "mul",
    "nor",
    "or",
    "sll",
    "sllv",
    "slt",
    "sltu",
    "sra",
    "srav", 
    "srl",
    "srlv",
    "sub",
    "subu",
    "syscall",
    "xor",
    "tge",
    "teq",
    "j",
    "jal",
    "addi",
    "addiu",
    "andi",
    "ori",
    "xori",
    "beq",
    "beql",
    "bne",
    "bnel",
    "bgtzl",
    "bgtz",
    "blez",
    "blezl",
    "bgez_bltz",
    "lui",
    "lh",
    "lb",
    "lbu",
    "lhu",
    "slti",
    "sltiu",
    "sw",
    "sh",
    "sb",
    "lwl",
    "lw",
    "ext",
    "seh",
    "ins",
    "clz",
    "swl",
    "swr",
    "sdc1",
    "ldc1",
    "bc1f",
    "bc1t",
    "bc1fl",
    "bc1tl",
    "lwc1",
    "swc1",
    "movci",
    "mfc1",
    "mtc1",
    "cvtd",
    "cvts",
    "truncl",
    "truncw",
    "lwr",
    "seb",
    "mfc0",
    "mtc0",
    "unknown"
  };


std::string getInstTypeStr(uint32_t idx)
{
  return instNames[idx];
}

std::string getGPRName(uint32_t r, bool spaces)
{
  r = r&31;
  if(spaces)
     return (r==0) ? regNames[r] : "  " + regNames[r];
  else
    return regNames[r];
}
std::string getCondName(uint32_t c)
{
  c = c & 15;
  return condNames[c];
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

static void _lwl(uint32_t inst, uint32_t addr, std::string &s);
static void _lwr(uint32_t inst, uint32_t addr, std::string &s);
static void _swl(uint32_t inst, uint32_t addr, std::string &s);
static void _swr(uint32_t inst, uint32_t addr, std::string &s);

/* Floating Point */
static void _c(uint32_t inst, uint32_t addr, std::string &s);
static void _sdc1(uint32_t inst, uint32_t addr, std::string &s);
static void _ldc1(uint32_t inst, uint32_t addr, std::string &s);
static void _fabs(uint32_t inst, uint32_t addr, std::string &s);
static void _fadd(uint32_t inst, uint32_t addr, std::string &s);
static void _fsub(uint32_t inst, uint32_t addr, std::string &s);
static void _fmul(uint32_t inst, uint32_t addr, std::string &s);
static void _fdiv(uint32_t inst, uint32_t addr, std::string &s);
static void _fmov(uint32_t inst, uint32_t addr, std::string &s);
static void _fneg(uint32_t inst, uint32_t addr, std::string &s);
static void _fsqrt(uint32_t inst, uint32_t addr, std::string &s);
static void _frsqrt(uint32_t inst, uint32_t addr, std::string &s);
static void _frecip(uint32_t inst, uint32_t addr, std::string &s);
static void _fmovc(uint32_t inst, uint32_t addr, std::string &s);
static void _bc1f(uint32_t inst, uint32_t addr, std::string &s);
static void _bc1t(uint32_t inst, uint32_t addr, std::string &s);
static void _bc1fl(uint32_t inst, uint32_t addr,  std::string &s);
static void _bc1tl(uint32_t inst, uint32_t addr, std::string &s);
static void _lwc1(uint32_t inst, uint32_t addr, std::string &s);
static void _swc1(uint32_t inst, uint32_t addr, std::string &s);
static void _movci(uint32_t inst, uint32_t addr, std::string &s);
static void _mfc1(uint32_t inst, uint32_t addr, std::string &s);
static void _mtc1(uint32_t inst, uint32_t addr, std::string &s);
static void _cvtd(uint32_t inst, uint32_t addr, std::string &s);
static void _cvts(uint32_t inst, uint32_t addr, std::string &s);
static void _truncl(uint32_t inst, uint32_t addr, std::string &s);
static void _truncw(uint32_t inst, uint32_t addr, std::string &s);

static void _ext(uint32_t inst, uint32_t addr, std::string &s);
static void _seh(uint32_t inst, uint32_t addr, std::string &s);
static void _ins(uint32_t inst, uint32_t addr, std::string &s);
static void _clz(uint32_t inst, uint32_t addr, std::string &s);

static void (*functTbl[64])(uint32_t inst, uint32_t addr, std::string &s) = {nullptr};
static void (*ITypeOpcodeTbl[64])(uint32_t inst, uint32_t addr, std::string &s) = {nullptr};

void initParseTables()
{
  /* These are R Type instructions (use function) */
  functTbl[0x00] = _sll;
  functTbl[0x01] = _movci;
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
 
  ITypeOpcodeTbl[0x01] = _bgez_bltz;
  ITypeOpcodeTbl[0x04] = _beq; 
  ITypeOpcodeTbl[0x05] = _bne;
  ITypeOpcodeTbl[0x06] = _blez;
  ITypeOpcodeTbl[0x07] = _bgtz;
  ITypeOpcodeTbl[0x14] = _beql;
  ITypeOpcodeTbl[0x15] = _bnel;
  ITypeOpcodeTbl[0x16] = _blezl;
  ITypeOpcodeTbl[0x17] = _bgtzl;
  
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
  ITypeOpcodeTbl[0x31] = _lwc1;  
  ITypeOpcodeTbl[0x39] = _swc1;

  ITypeOpcodeTbl[0x2a] = _swl;
  ITypeOpcodeTbl[0x2e] = _swr;
  ITypeOpcodeTbl[0x22] = _lwl;
  ITypeOpcodeTbl[0x26] = _lwr;
    
}

bool isBranchOrJump(uint32_t inst)
{
  uint32_t opcode = inst>>26;
  uint32_t funct = inst & 63;
  bool isRType = (opcode==0);
  bool isJType = ((opcode>>1)==1);
  bool isCoproc0 = (opcode == 0x10);
  bool isCoproc1 = (opcode == 0x11);
  bool isCoproc2 = (opcode == 0x12);
  bool isSpecial2 = (opcode == 0x1c); 
  bool isSpecial3 = (opcode == 0x1f);

  if(isRType){
    if(funct == 0x08)
      return true;
    else if(funct == 0x09)
      return true;
    else
      return false;
  }
  else if(isSpecial2)
    return false;
  else if(isSpecial3)
    return false;
  else if(isJType)
    return true;
  else if(isCoproc0)
    return false;
  else if(isCoproc1) {
    uint32_t fmt = (inst >> 21) & 31;
    return (fmt == 0x8);
  }
  else if(isCoproc2)
    return false;
  else 
    {
      switch(opcode)
	{
	case 0x01:
	  //_bgez_bltz;
	  return true;
	  break;
	case 0x04:
	  //_beq
	  return true;
	  break;
	case 0x05:
	  //_bne
	  return true;
	  break;
	case 0x06:
	  //_blez
	  return true;
	  break;
	case 0x07:
	  //_bgtz
	  return true;
	  break;
	case 0x14:
	  //_beql
	  return true;
	  break;
	case 0x15:
	  //_bnel
	  return true;
	  break;
	case 0x16:
	  //blezl
	  return true;
	  break;
	case 0x17:
	  //bgtzl
	  return true;
	  break;
	default:
	  return false;
	  break;
	}
    }
}

bool isFloatingPoint(uint32_t inst) {
  uint32_t opcode = inst>>26;
  return (opcode == 0x11);
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
  else if(funct == 0x4)
    {
      _ins(inst, addr, s);
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
static std::string decodeCoproc1(uint32_t inst,uint32_t addr) {
  uint32_t functField = (inst>>21) & 31;
  uint32_t lowop = inst & 63;  
  uint32_t fmt = (inst >> 21) & 31;
  uint32_t nd_tf = (inst>>16) & 3;
  
  uint32_t lowbits = inst & ((1<<11)-1);
  std::string s;
  
  if(fmt == 0x8) {
    switch(nd_tf) {
    case 0x0:
      _bc1f(inst, addr, s);
      break;
    case 0x1:
      _bc1t(inst, addr, s);
      break;
    case 0x2:
      _bc1fl(inst, addr, s);
      break;
    case 0x3:
      _bc1tl(inst, addr, s);
      break;
    }
    /*BRANCH*/
  }
 else if((lowbits == 0) && ((functField==0x0) || (functField==0x4))) {
   if(functField == 0x0) {
     /* move from coprocessor */
     _mfc1(inst, addr, s);
   }
   else if(functField == 0x4) {
     /* move to coprocessor */
     _mtc1(inst, addr, s);
   }
 }
 else {
   if((lowop >> 4) == 3) {
     _c(inst, addr, s);
   }
   else {
     switch(lowop) {
     case 0x0:
       _fadd(inst, addr, s);
       break;
     case 0x1:
       _fsub(inst, addr, s);
       break;
     case 0x2:
       _fmul(inst, addr, s);
       break;
     case 0x3:
       _fdiv(inst, addr, s);
       break;
     case 0x4:
       _fsqrt(inst, addr, s);
       break;
     case 0x5:
       _fabs(inst, addr, s);
       break;
     case 0x6:
       _fmov(inst, addr, s);
       break;
     case 0x7:
       _fneg(inst, addr, s);
       break;
     case 0x9:
       _truncl(inst, addr, s);
       break;
     case 0xd:
       _truncw(inst, addr, s);
       break;
     case 0x11:
       _fmovc(inst, addr, s);
       break;
     case 0x15:
       _frecip(inst, addr, s);
       break;
     case 0x16:
       _frsqrt(inst, addr, s);
       break;
     case 0x20:
       /* cvt.s */
       _cvts(inst, addr, s);
       break;
     case 0x21:
       _cvtd(inst, addr, s);
       break;
     default:
       printf("unhandled coproc1 instruction (%x) @ %08x\n", inst, addr);
       exit(-1);
       break;
     }
   }
 }
  return s;
}
static std::string decodeCoproc2(uint32_t inst,uint32_t addr)
{
  std::string s;
  uint32_t opcode = inst>>26;
  uint32_t functField = (inst>>21) & 31;
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

static void _tge(uint32_t inst, uint32_t addr, std::string &s) {
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  s += "tge " + regNames[rs] + "," + regNames[rt];
}

static void _teq(uint32_t inst, uint32_t addr, std::string &s) {
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
      /* bltzl : less than zero likely */
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

static void _bgtz(uint32_t inst, uint32_t addr, std::string &s) {
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
  int32_t npc = addr+4; 
  s += "bgtz " + regNames[rs] + "," + toStringHex(imm+npc);
}

static void _bgtzl(uint32_t inst, uint32_t addr, std::string &s) {
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
  int32_t npc = addr+4; 
  s += "bgtzl " + regNames[rs] + "," + toStringHex(imm+npc);
}

static void _blez(uint32_t inst, uint32_t addr, std::string &s) {
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
  int32_t npc = addr+4; 
  s += "blez " + regNames[rs] + "," + toStringHex(imm+npc);
}

static void _blezl(uint32_t inst, uint32_t addr, std::string &s) {
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
  int32_t npc = addr+4; 
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

static void _ins(uint32_t inst, uint32_t addr,std::string &s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  uint32_t lsb = (inst >> 6) & 31;
  uint32_t msb = ((inst >> 11) & 31);
  s += "ins " + regNames[rt] + "," + regNames[rs] + ",0x" + toStringHex(msb) + ",0x" + toStringHex(lsb);
  /* store in rt */
}

static void _seh(uint32_t inst, uint32_t addr,std::string &s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  s += "seh " + regNames[rd] + "," + regNames[rt];
}

static void _clz(uint32_t inst, uint32_t addr,std::string &s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  s += "clz " + regNames[rd] + "," + regNames[rt];
}


/* FLOATING POINT */
static void _c(uint32_t inst, uint32_t addr, std::string &s)
{
  uint32_t fmt = (inst >> 21) & 31;
  uint32_t cond = inst & 15;
  uint32_t cc = (inst >> 8) & 7;
  uint32_t ft = (inst >> 16) & 31;
  uint32_t fs = (inst >> 11) & 31;
  
  std::string fmtS = ".?.";
  switch(fmt)
    {
    case FMT_S:
      fmtS = ".s";
      break;
    case FMT_D:
      fmtS = ".d";
      break;
    default:
      break;
    }

  s += "c." + getCondName(cond) + fmtS + " fcc" + toString(cc) +
    ","+ "$f"+toString(fs) + "," + "$f"+toString(ft);
}


static void _ldc1(uint32_t inst, uint32_t addr,std::string &s)
{
  uint32_t ft = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  
  s += "ldc1 $f" + toString(ft) + "," +  toString<int32_t>(imm) +
    "(" + regNames[rs] + ")";
}
static void _sdc1(uint32_t inst, uint32_t addr,std::string &s)
{
  uint32_t ft = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  
  s += "sdc1 $f" + toString(ft) + "," +  toString<int32_t>(imm) +
    "(" + regNames[rs] + ")";
}
static void _fabs(uint32_t inst, uint32_t addr, std::string &s)
{s = std::string(__func__); }
static void _fadd(uint32_t inst, uint32_t addr, std::string &s)
{
  uint32_t fmt = (inst >> 21) & 31;
  uint32_t ft = (inst >> 16) & 31;
  uint32_t fs = (inst >> 11) & 31;
  uint32_t fd = (inst >> 6) & 31;

  switch(fmt)
    {
    case FMT_S:
      s = "add.s ";
      break;
    case FMT_D:
      s = "add.d ";
      break;
    default:
      s = "add.?? ";
      break;
    }
  s += "$f"+toString(fd) + ",$f"+toString(fs) + ",$f" + toString(ft); 
}

static void _fsub(uint32_t inst, uint32_t addr, std::string &s)
{
  uint32_t fmt = (inst >> 21) & 31;
  uint32_t ft = (inst >> 16) & 31;
  uint32_t fs = (inst >> 11) & 31;
  uint32_t fd = (inst >> 6) & 31;

  switch(fmt)
    {
    case FMT_S:
      s = "sub.s ";
      break;
    case FMT_D:
      s = "sub.d ";
      break;
    default:
      s = "sub.?? ";
      break;
    }
  s += "$f"+toString(fd) + ",$f"+toString(fs) + ",$f" + toString(ft); 
}

static void _fmul(uint32_t inst, uint32_t addr, std::string &s)
{
  uint32_t fmt = (inst >> 21) & 31;
  uint32_t ft = (inst >> 16) & 31;
  uint32_t fs = (inst >> 11) & 31;
  uint32_t fd = (inst >> 6) & 31;

  switch(fmt)
    {
    case FMT_S:
      s = "mul.s ";
      break;
    case FMT_D:
      s = "mul.d ";
      break;
    default:
      s = "mul.?? ";
      break;
    }
  s += "$f"+toString(fd) + ",$f"+toString(fs) + ",$f" + toString(ft); 
}
static void _fdiv(uint32_t inst, uint32_t addr, std::string &s)
{
  uint32_t fmt = (inst >> 21) & 31;
  uint32_t ft = (inst >> 16) & 31;
  uint32_t fs = (inst >> 11) & 31;
  uint32_t fd = (inst >> 6) & 31;

  switch(fmt)
    {
    case FMT_S:
      s = "div.s ";
      break;
    case FMT_D:
      s = "div.d ";
      break;
    default:
      s = "div.?? ";
      break;
    }
  s += "$f"+toString(fd) + ",$f"+toString(fs) + ",$f" + toString(ft); 
}

static void _fmov(uint32_t inst, uint32_t addr, std::string &s)
{
  uint32_t fmt = (inst >> 21) & 31;
  uint32_t fd = (inst>>6) & 31;
  uint32_t fs = (inst>>11) & 31;

  switch(fmt)
    {
    case FMT_S:
      s = "mov.s ";
      break;
    case FMT_D:
      s = "mov.d ";
      break;
    default:
      s = "mov.?? ";
      break;
    }
  s += "$f"+toString(fd) + "," + "$f"+toString(fs); 
}
static void _fneg(uint32_t inst, uint32_t addr, std::string &s)
{s = std::string(__func__); }
static void _fsqrt(uint32_t inst, uint32_t addr, std::string &s)
{
  uint32_t fmt = (inst >> 21) & 31;
  uint32_t fs = (inst >> 11) & 31;
  uint32_t fd = (inst >> 6) & 31;
  switch(fmt)
    {
    case FMT_S:
      s = "sqrt.s ";
      break;
    case FMT_D:
      s = "sqrt.d ";
      break;
    default:
      s = "sqrt.?? ";
      break;
    }
  s += "$f"+toString(fd)+",$f"+toString(fs);
}
static void _frsqrt(uint32_t inst, uint32_t addr, std::string &s)
{s = std::string(__func__); }
static void _frecip(uint32_t inst, uint32_t addr, std::string &s)
{s = std::string(__func__); }
static void _fmovc(uint32_t inst, uint32_t addr, std::string &s)
{
  uint32_t cc = (inst >> 18) & 7;
  uint32_t fd = (inst>>6) & 31;
  uint32_t fs = (inst>>11) & 31;
  uint32_t tf = (inst>>16) & 1;
  uint32_t fmt = (inst >> 21) & 31;
  switch(fmt)
    {
    case FMT_S:
      s = tf ? "movc.t.s " : "movc.f.s ";
      break;
    case FMT_D:
      s = tf ? "movc.t.d " : "movc.f.d ";
      break;
    default:
      s = tf ? "movc.t.?? " : "movc.f.?? ";
      break;
    }
  s += "fcc"+toString(cc)+",$f"+toString(fd)+",$f"+toString(fs);
}
static void _bc1f(uint32_t inst, uint32_t addr, std::string &s)
{
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
  int32_t npc = addr+4; 
  uint32_t cc = (inst >> 18) & 7;
  s = "bc1f fcc" + toString(cc) + "," + toStringHex(imm+npc);

}
static void _bc1t(uint32_t inst, uint32_t addr, std::string &s)
{
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
  int32_t npc = addr+4; 
  uint32_t cc = (inst >> 18) & 7;
  s = "bc1t fcc" + toString(cc) + "," + toStringHex(imm+npc);
}
static void _bc1fl(uint32_t inst, uint32_t addr,  std::string &s)
{
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
  int32_t npc = addr+4; 
  uint32_t cc = (inst >> 18) & 7;
  s = "bc1fl fcc" + toString(cc) + "," + toStringHex(imm+npc);
}
static void _bc1tl(uint32_t inst, uint32_t addr, std::string &s)
{
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
  int32_t npc = addr+4; 
  uint32_t cc = (inst >> 18) & 7;
  s = "bc1tl fcc" + toString(cc) + "," + toStringHex(imm+npc);
}
static void _lwc1(uint32_t inst, uint32_t addr, std::string &s)
{
  uint32_t ft = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  
  s += "lwc1 $f" + toString(ft) + "," +  toString<int32_t>(imm) +
    "(" + regNames[rs] + ")";
}
static void _swc1(uint32_t inst, uint32_t addr, std::string &s)
{
  uint32_t ft = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  
  s += "swc1 $f" + toString(ft) + "," +  toString<int32_t>(imm) +
    "(" + regNames[rs] + ")";
}
static void _movci(uint32_t inst, uint32_t addr, std::string &s)
{s = std::string(__func__); }
static void _mfc1(uint32_t inst, uint32_t addr, std::string &s)
{
  uint32_t fd = (inst>>11) & 31;
  uint32_t rt = (inst>>16) & 31;
  s += "mfc1 " + regNames[rt] + ",$f" + toString(fd);
}

static void _mtc1(uint32_t inst, uint32_t addr, std::string &s)
{
  uint32_t fd = (inst>>11) & 31;
  uint32_t rt = (inst>>16) & 31;
  s += "mtc1 $f" + toString(fd) +"," + regNames[rt];
}
static void _cvtd(uint32_t inst, uint32_t addr, std::string &s)
{ 
  uint32_t fmt = (inst >> 21) & 31;
  uint32_t fd = (inst>>6) & 31;
  uint32_t fs = (inst>>11) & 31;
  switch(fmt)
    {
    case FMT_S:
      s = "cvt.s.d ";
      break;
    case FMT_W:
      s = "cvt.w.d ";
      break;
    default:
      s = "cvt.?.d ";
      break;
    }
  s += "$f"+toString(fd) +",$f"+toString(fs);
}
static void _cvts(uint32_t inst, uint32_t addr, std::string &s)
{ 
  uint32_t fmt = (inst >> 21) & 31;
  uint32_t fd = (inst>>6) & 31;
  uint32_t fs = (inst>>11) & 31;
  switch(fmt)
    {
    case FMT_D:
      s = "cvt.d.s ";
      break;
    case FMT_W:
      s = "cvt.w.s ";
      break;
    default:
      s = "cvt.?.s ";
      break;
    }
  s += "$f"+toString(fd) +",$f"+toString(fs);
}
static void _truncl(uint32_t inst, uint32_t addr, std::string &s)
{
  s = std::string(__func__); 
}

static void _truncw(uint32_t inst, uint32_t addr, std::string &s)
{
  uint32_t fmt = (inst >> 21) & 31;
  uint32_t fd = (inst>>6) & 31;
  uint32_t fs = (inst>>11) & 31;
  switch(fmt)
    {
    case FMT_S:
      s = "trunc.w.s = ";
      break;
    case FMT_D:
      s = "trunc.w.d = ";
      break;
    default:
      s = "trunc.w.?? = ";
      break;
    }
  s += "$f"+toString(fd) +",$f"+toString(fs);
}


static void _lwl(uint32_t inst, uint32_t addr, std::string &s)
{s = std::string(__func__); }
static void _lwr(uint32_t inst, uint32_t addr, std::string &s)
{s = std::string(__func__); }
static void _swl(uint32_t inst, uint32_t addr, std::string &s)
{s = std::string(__func__); }
static void _swr(uint32_t inst, uint32_t addr, std::string &s)
{s = std::string(__func__); }


