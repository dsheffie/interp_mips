#include "profileMips.hh"
#include "parseMips.hh"
#include "basicBlock.hh"

#include "helper.hh"
#include "simCache.hh"
#include "globals.hh"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <map>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>

#define K1SIZE  (0x20000000)
#define __USE_EX_FUNCTS__ 1

typedef struct
{
  uint32_t tv_sec;
  uint32_t tv_usec;
} timeval32_t;

typedef struct
{
  uint32_t tms_utime;
  uint32_t tms_stime;
  uint32_t tms_cutime;
  uint32_t tms_cstime;
} tms32_t;

typedef struct
{
  uint16_t st_dev;
  uint16_t st_ino;
  uint32_t st_mode;
  uint16_t st_nlink;
  uint16_t st_uid;
  uint16_t st_gid;
  uint16_t st_rdev;
  uint32_t st_size;
  uint32_t _st_atime;
  uint32_t st_spare1;
  uint32_t _st_mtime;
  uint32_t st_spare2;
  uint32_t _st_ctime;
  uint32_t st_spare3;
  uint32_t st_blksize;
  uint32_t st_blocks;
  uint32_t st_spare4[2];
} stat32_t;

static timeval32_t myTimeVal;
static uint32_t myTime = 1<<20;

basicBlock *cBB;

static state_t otherState;

static void getNextBlock(state_t *s);

exFunct getExecRType(uint32_t inst, state_t *s);
exFunct getExecJType(uint32_t inst, state_t *s);
exFunct getExecIType(uint32_t inst, state_t *s);
exFunct getExecSpecial2(uint32_t inst, state_t *s);
exFunct getExecSpecial3(uint32_t inst, state_t *s);
exFunct getExecCoproc0(uint32_t inst, state_t *s);
exFunct getExecCoproc1(uint32_t inst, state_t *s);
exFunct getExecCoproc2(uint32_t inst, state_t *s);

void execRType(uint32_t inst, state_t *s);
void execJType(uint32_t inst, state_t *s);
void execIType(uint32_t inst, state_t *s);
void execSpecial2(uint32_t inst, state_t *s);
void execSpecial3(uint32_t inst, state_t *s);
void execCoproc0(uint32_t inst, state_t *s);
void execCoproc1(uint32_t inst, state_t *s);
void execCoproc2(uint32_t inst, state_t *s);

static uint32_t getConditionCode(state_t *s, uint32_t cc);
static void setConditionCode(state_t *s, uint32_t v, uint32_t cc);

/* RType instructions */
static void _monitor(uint32_t inst, state_t *s);
static void _monitorBody(uint32_t inst, state_t *s);
static void _sync(uint32_t inst, state_t *s);

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
static void _maddu(uint32_t inst, state_t *s);

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
static void _bgtzl(uint32_t inst, state_t *s);
static void _blez(uint32_t inst, state_t *s);
static void _blezl(uint32_t inst, state_t *s);

static void _bgez_bltz(uint32_t inst, state_t *s);
static void _lui(uint32_t inst, state_t *s);

static void _lw(uint32_t inst, state_t *s);
static void _lh(uint32_t inst, state_t *s);
static void _lb(uint32_t inst, state_t *s);
static void _lbu(uint32_t inst, state_t *s);
static void _lhu(uint32_t inst, state_t *s);
static void _ldc1(uint32_t inst, state_t *s);
static void _lwc1(uint32_t inst, state_t *s);

static void _slti(uint32_t inst, state_t *s);
static void _sltiu(uint32_t inst, state_t *s);
static void _sw(uint32_t inst, state_t *s);
static void _sh(uint32_t inst, state_t *s);
static void _sb(uint32_t inst, state_t *s);
static void _sdc1(uint32_t inst, state_t *s);
static void _swc1(uint32_t inst, state_t *s);

/* These access the cache model */
static void _lwSim(uint32_t inst, state_t *s);
static void _lhSim(uint32_t inst, state_t *s);
static void _lbSim(uint32_t inst, state_t *s);
static void _lbuSim(uint32_t inst, state_t *s);
static void _lhuSim(uint32_t inst, state_t *s);
static void _ldc1Sim(uint32_t inst, state_t *s);
static void _lwc1Sim(uint32_t inst, state_t *s);

static void _swSim(uint32_t inst, state_t *s);
static void _shSim(uint32_t inst, state_t *s);
static void _sbSim(uint32_t inst, state_t *s);
static void _sdc1Sim(uint32_t inst, state_t *s);
static void _swc1Sim(uint32_t inst, state_t *s);

static void _llSim(uint32_t inst, state_t *s);
static void _scSim(uint32_t inst, state_t *s);
static void _lwlSim(uint32_t inst, state_t *s);
static void _lwrSim(uint32_t inst, state_t *s);
static void _swlSim(uint32_t inst, state_t *s);
static void _swrSim(uint32_t inst, state_t *s);


static void _seh(uint32_t inst, state_t *s);
static void _seb(uint32_t inst, state_t *s);
static void _ext(uint32_t inst, state_t *s);
static void _ins(uint32_t inst, state_t *s);
static void _clz(uint32_t inst, state_t *s);

static void _mtc0(uint32_t inst, state_t *s);
static void _mfc0(uint32_t inst, state_t *s);

static void _mtc1(uint32_t inst, state_t *s);
static void _mfc1(uint32_t inst, state_t *s);

/* The profiling versions of this functions
 * change the cBB, these versions do not
 * and therefore are used in emulated
 * execution */
static void _jalrEx(uint32_t inst, state_t *s);
static void _jrEx(uint32_t inst, state_t *s);
static void _jEx(uint32_t inst, state_t *s);
static void _jalEx(uint32_t inst, state_t *s);
static void _beqEx(uint32_t inst, state_t *s);
static void _beqlEx(uint32_t inst, state_t *s);
static void _bneEx(uint32_t inst, state_t *s);
static void _bnelEx(uint32_t inst, state_t *s);
static void _bgtzEx(uint32_t inst, state_t *s);
static void _bgtzlEx(uint32_t inst, state_t *s);
static void _blezEx(uint32_t inst, state_t *s);
static void _blezlEx(uint32_t inst, state_t *s);
static void _bgez_bltzEx(uint32_t inst, state_t *s);
static void _monitorEx(uint32_t inst, state_t *s);
static void _bc1fEx(uint32_t inst, state_t *s);
static void _bc1tEx(uint32_t inst, state_t *s);
static void _bc1flEx(uint32_t inst, state_t *s);
static void _bc1tlEx(uint32_t inst, state_t *s);

static void _lwl(uint32_t inst, state_t *s);
static void _lwr(uint32_t inst, state_t *s);
static void _swl(uint32_t inst, state_t *s);
static void _swr(uint32_t inst, state_t *s);

static void _ll(uint32_t inst, state_t *s);
static void _sc(uint32_t inst, state_t *s);

/* FLOATING-POINT */
static void _c(uint32_t inst, state_t *s);
static void _cs(uint32_t inst, state_t *s);
static void _cd(uint32_t inst, state_t *s);

static void _cvts(uint32_t inst, state_t *s);
static void _cvtd(uint32_t inst, state_t *s);

static void _truncw(uint32_t inst, state_t *s);
static void _truncl(uint32_t inst, state_t *s);


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

/* Profiling tables */
static void (*functTbl[64])(uint32_t inst, state_t *s) = {NULL};
static void (*ITypeOpcodeTbl[64])(uint32_t inst, state_t *s) = {NULL};

/* Execution tables */
static void (*functTblEx[64])(uint32_t inst, state_t *s) = {NULL};
static void (*ITypeOpcodeTblEx[64])(uint32_t inst, state_t *s) = {NULL};

static bool enTimingFuncts = false;
static int sArgc;
static char** sArgv;

void hashState(state_t *s)
{
}

void hashState(state_t *s, FILE *fp)
{
}

void printState(state_t *s)
{
  printf("PC=0x%08x\n", s->pc);
  for(int32_t i = 0; i < 32; i++)
    {
      printf("%s: 0x%08x (%d)\n", 
	     getGPRName(i,true).c_str(), 
	     s->gpr[i], 
	     s->gpr[i]);  
    }
  printf("  lo: 0x%08x (%d)\n", s->lo, s->lo);
  printf("  hi: 0x%08x (%d)\n", s->hi, s->hi);
  for(int32_t i = 0; i < 32; i++)
    {
      printf("$f%d : %x\n", i, s->cpr1[i]);
    }
  for(int32_t i = 0; i < 5; i++)
    {
      printf("fcr%d : %x\n", i, s->fcr1[i]);
    }
}

void compareState(state_t *state0, state_t *state1)
{
  if(state0->pc != state1->pc) {
    printf("PCs do not match, state0->pc = %x, state1->pc = %x\n", 
	   state0->pc, state1->pc);
  }
  for(int32_t i = 0; i < 32; i++)
    {
      if(state0->gpr[i] != state1->gpr[i]) {
	const char *rName = getGPRName(i,true).c_str();
	printf("%s do not match, state0 = %x, state1 = %x\n",
	       rName, state0->gpr[i], state1->gpr[i]);
      }
    }
  if(state0->lo != state1->lo) {
    printf("LO register mismatch, state0 = %x, state1 = %x\n",
	   state0->lo, state1->lo);
  }
  if(state0->hi != state1->hi) {
    printf("HI register mismatch, state0 = %x, state1 = %x\n",
	   state0->hi, state1->hi);
  }
  for(int32_t i = 0; i < 32; i++)
    {
      if(state0->cpr1[i] != state1->cpr1[i]) {
	std::string rName = "$f" + toString(i);
	printf("%s do not match, state0 = %x, state1 = %x\n",
	       rName.c_str(), state0->cpr1[i], state1->cpr1[i]);
      }
    }
  for(int32_t i = 0; i < 5; i++)
    {
      if(state0->fcr1[i] != state1->fcr1[i]) {
	std::string rName = "$fcr" + toString(i);
	printf("%s do not match, state0 = %x, state1 = %x\n",
	       rName.c_str(), state0->fcr1[i], state1->fcr1[i]);
      }
    }

}

static uint32_t getConditionCode(state_t *s, uint32_t cc)
{
  return ((s->fcr1[CP1_CR25] & (1U<<cc)) >> cc) & 0x1;
}
static void setConditionCode(state_t *s, uint32_t v, uint32_t cc)
{
  uint32_t m0,m1,m2;
  m0 = 1U<<cc;
  m1 = ~m0;
  m2 = ~(v-1);
  s->fcr1[CP1_CR25] = (s->fcr1[CP1_CR25] & m1) | ((1U<<cc) & m2);
}

void initEmulationTables(bool enClockFuncts, int sysArgc, char **sysArgv)
{
  memset(&myTimeVal, 0, sizeof(myTimeVal));
  sArgc = sysArgc;
  sArgv = sysArgv;
  enTimingFuncts = enClockFuncts;
  /* These are R Type instructions (use function) */
  functTbl[0x00] = _sll;
  functTbl[0x01] = _movci;
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
  functTbl[0x0f] = _sync;
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
  ITypeOpcodeTbl[0x17] = _bgtzl;
  
  ITypeOpcodeTbl[0x06] = _blez;
  ITypeOpcodeTbl[0x16] = _blezl;
  ITypeOpcodeTbl[0x07] = _bgtz;

  ITypeOpcodeTbl[0x01] = _bgez_bltz;

  ITypeOpcodeTbl[0x0A] = _slti;
  ITypeOpcodeTbl[0x0B] = _sltiu;

  ITypeOpcodeTbl[0x0F] = _lui;
  ITypeOpcodeTbl[0x20] = dCache ? _lbSim : _lb;
  ITypeOpcodeTbl[0x21] = dCache ? _lhSim : _lh;
  ITypeOpcodeTbl[0x23] = dCache ? _lwSim : _lw;

  ITypeOpcodeTbl[0x24] = dCache ? _lbuSim : _lbu;
  ITypeOpcodeTbl[0x25] = dCache ? _lhuSim : _lhu;
 
  ITypeOpcodeTbl[0x28] = dCache ? _sbSim : _sb;
  ITypeOpcodeTbl[0x29] = dCache ? _shSim : _sh;
  ITypeOpcodeTbl[0x2B] = dCache ? _swSim : _sw;
  
  ITypeOpcodeTbl[0x3D] = dCache ? _sdc1Sim : _sdc1;
  ITypeOpcodeTbl[0x35] = dCache ? _ldc1Sim : _ldc1;
  ITypeOpcodeTbl[0x31] = dCache ? _lwc1Sim : _lwc1;  
  ITypeOpcodeTbl[0x39] = dCache ? _swc1Sim : _swc1;

  ITypeOpcodeTbl[0x2a] = dCache ? _swlSim : _swl;
  ITypeOpcodeTbl[0x2e] = dCache ? _swrSim : _swr;
  ITypeOpcodeTbl[0x22] = dCache ? _lwlSim : _lwl;
  ITypeOpcodeTbl[0x26] = dCache ? _lwrSim : _lwr;

  memcpy(functTblEx, functTbl, sizeof(functTbl[0])*64);
  memcpy(ITypeOpcodeTblEx, ITypeOpcodeTbl, sizeof(ITypeOpcodeTbl[0])*64);

#ifdef __USE_EX_FUNCTS__
  functTblEx[0x05] = _monitorEx;
  functTblEx[0x08] = _jrEx;
  functTblEx[0x09] = _jalrEx;
  ITypeOpcodeTblEx[0x01] = _bgez_bltzEx;
  ITypeOpcodeTblEx[0x04] = _beqEx;
  ITypeOpcodeTblEx[0x05] = _bneEx;
  ITypeOpcodeTblEx[0x06] = _blezEx;
  ITypeOpcodeTblEx[0x16] = _blezlEx;
  ITypeOpcodeTblEx[0x07] = _bgtzEx;
  ITypeOpcodeTblEx[0x14] = _beqlEx; 
  ITypeOpcodeTblEx[0x15] = _bnelEx;
  ITypeOpcodeTblEx[0x17] = _bgtzlEx;
#endif

 }


exFunct decForExec(uint32_t inst, state_t *s)
{
  exFunct funct  = 0;  
  uint32_t opcode = inst>>26;
  bool isRType = (opcode==0);
  bool isJType = ((opcode>>1)==1);
  bool isCoproc0 = (opcode == 0x10);
  bool isCoproc1 = (opcode == 0x11);
  bool isCoproc2 = (opcode == 0x12);
  bool isSpecial2 = (opcode == 0x1c); 
  bool isSpecial3 = (opcode == 0x1f);
  bool isLoadLinked = (opcode == 0x30);
  bool isStoreCond = (opcode == 0x38);
  
  if(isRType)
    funct = getExecRType(inst,s);
  else if(isSpecial2)
    funct = getExecSpecial2(inst,s);
  else if(isSpecial3)
    funct = getExecSpecial3(inst,s);
  else if(isJType)
    funct = getExecJType(inst,s);
  else if(isCoproc0)
    funct = getExecCoproc0(inst,s);
  else if(isCoproc1)
    funct = getExecCoproc1(inst,s);
  else if(isCoproc2)
    funct = getExecCoproc2(inst,s);
  else if(isLoadLinked)
    funct = _ll;
  else if(isStoreCond)
    funct = _sc;
  else 
    funct = getExecIType(inst,s);

  assert(funct!=0);
  return funct;

}
bool getStaticBranchTarget(uint32_t inst, uint32_t pc, uint32_t &bTarget)
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

  /* J Type instructions */
  uint32_t jaddr = inst & ((1<<26)-1);
  jaddr <<= 2;
  jaddr |= ((pc+4) & (~((1<<28)-1)));
  /* I Type instructions */
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
  int32_t iaddr = imm+pc+4;

  if(isRType) {
    //functTbl[0x08] = _jr;
    //functTbl[0x09] = _jalr;
    return false;
  }
  else if(isSpecial2)
    return false;
  else if(isSpecial3)
    return false;
  else if(isJType)
    {
      if(opcode==0x2)
	{
	  /* j */
	  bTarget = jaddr;
	  return true;
	}
      else if(opcode==0x3)
	{
	  /* jal */
	  bTarget = jaddr;
	  return true;
	}
      else
	{
	  return false;
	}
    }
  else if(isCoproc0)
    return false;
  else if(isCoproc1)
    {
      uint32_t fmt = (inst >> 21) & 31;
      if(fmt == 0x8)
	{
	  bTarget = iaddr;
	  return true;
	}
      else
	{
	  return false;
	}
    }
  else if(isCoproc2)
    return false;
  else 
    {
      switch(opcode)
	{
	  //ITypeOpcodeTbl[0x01] = _bgez_bltz;
	case 0x01:
	  bTarget = iaddr;
	  return true;
	  break;
	  //ITypeOpcodeTbl[0x04] = _beq;
	case 0x04:
	  bTarget = iaddr;
	  return true;
	  break;
	  //ITypeOpcodeTbl[0x05] = _bne;
	case 0x05:
	  bTarget = iaddr;
	  return true;
	  break;
	  //ITypeOpcodeTbl[0x06] = _blez;
	case 0x06:
	  bTarget = iaddr;
	  return true;
	  break;
	  //ITypeOpcodeTbl[0x07] = _bgtz;
	case 0x07:
	  bTarget = iaddr;
	  return true;
	  break;
	  //ITypeOpcodeTbl[0x14] = _beql;
	case 0x14:
	  bTarget = iaddr;
	  return true;
	  break;
	  //ITypeOpcodeTbl[0x15] = _bnel;
	case 0x15:
	  bTarget = iaddr;
	  return true;
	  break;
	  //ITypeOpcodeTbl[0x17] = _bgtzl;
	case 0x17:
	  bTarget = iaddr;
	  return true;
	  break;
	default:
	  return false;
	}
    }
}

void execMips(state_t *s)
{
  uint8_t *mem = s->mem;
  uint32_t inst = accessBigEndian(*(uint32_t*)(mem + s->pc));
  uint32_t opcode = inst>>26;
  bool isRType = (opcode==0);
  bool isJType = ((opcode>>1)==1);
  bool isCoproc0 = (opcode == 0x10);
  bool isCoproc1 = (opcode == 0x11);
  bool isCoproc2 = (opcode == 0x12);
  bool isSpecial2 = (opcode == 0x1c); 
  bool isSpecial3 = (opcode == 0x1f);
  bool isLoadLinked = (opcode == 0x30);
  bool isStoreCond = (opcode == 0x38);

#ifdef __ENABLE_LOGGING__
  if(executeLog) {
    std::string ss = getAsmString(inst, s->pc);
    *executeLog += toStringHex(s->pc) + ": " + ss + "\n";
  }
#endif

  if(s->pc < ENTRY_POINT) {
    printf("s->pc = %x is before entry point!!\n", s->pc);
    exit(-1);
  }
  cBB->addIns(inst, s->pc);
  s->icnt++;
  
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
  else if(isLoadLinked)
    _ll(inst, s);
  else if(isStoreCond)
    _sc(inst, s);
  else 
    execIType(inst,s);

  if(s->gpr[0] != 0)
    {
      printf("pc=%x, s->gpr[0] = %x\n", s->pc, s->gpr[0]);
      exit(-1);
    }
}

exFunct getExecSpecial2(uint32_t inst,state_t *s)
{
  uint32_t funct = inst & 63;
  exFunct eF = 0;
  switch(funct)
    {
    case(0x0):
      //_madd(inst,s);
      eF = _madd;
      break;
    case 0x1:
      eF = _maddu;
      break;
    case(0x2):
      //_mul(inst,s);
      eF = _mul;
      break;
    case(0x20):
      //_clz(inst, s);
      eF = _clz;
      break;
    default:
      printf("unhandled special2 instruction @ 0x%08x\n", s->pc); 
      exit(-1);
      break;
    }
  return eF;
}

void execSpecial2(uint32_t inst,state_t *s)
{
  uint32_t funct = inst & 63;
  switch(funct)
    {
    case(0x0):
      _madd(inst,s);
      break;
    case 0x1:
      _maddu(inst,s);
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
}
exFunct getExecSpecial3(uint32_t inst,state_t *s)
{
  uint32_t funct = inst & 63;
  uint32_t op = (inst>>6) & 31;
  exFunct eF = 0;
  if(funct == 32)
    {
      switch(op)
	{
	case 0x10:
	  eF = _seb;
	  break;
	case 0x18:
	  //_seh(inst, s);
	  eF = _seh;
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
      //_ext(inst, s);
      eF = _ext;
    }
  else if(funct == 0x4)
    {
      eF = _ins;
    }
  else
    {
      eF = 0;
    }
  //_seh;
  return eF;
}
void execSpecial3(uint32_t inst,state_t *s)
{
  uint32_t funct = inst & 63;
  uint32_t op = (inst>>6) & 31;
  if(funct == 32)
    {
      switch(op)
	{
	case 0x10:
	  _seb(inst, s);
	  break;
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
  else if(funct == 0x4)
    {
      _ins(inst, s);
    }
  else
    {
      printf("unhandled special3 instruction @ 0x%08x\n", s->pc); 
      exit(-1);    
    }
}
exFunct getExecRType(uint32_t inst,state_t *s)
{
  uint32_t funct = inst & 63;
  exFunct eF = 0;
  if(functTblEx[funct] == 0)
    {
      printf("unknown RType instruction %x, funct = %d\n", s->pc, funct);
      exit(-1);
      return 0;
    }
  else
    {
      eF = functTblEx[funct];
      //functTbl[funct](inst, s);
    }
  return eF;
}
void execRType(uint32_t inst,state_t *s)
{
  uint32_t funct = inst & 63;
  if(functTbl[funct] == 0)
    {
      printf("unknown RType instruction %x, funct = %d\n", s->pc, funct);
      exit(-1);
    }
  else
    {
      functTbl[funct](inst, s);
    }
}
exFunct getExecJType(uint32_t inst, state_t *s)
{
  uint32_t opcode = inst>>26;
  exFunct eF = 0;
  if(opcode==0x2)
    {
      //_j(inst,s)
#ifdef __USE_EX_FUNCTS__
      eF = _jEx;
#else
      eF = _j;
#endif
    }
  else if(opcode==0x3)
    {
      //_jal(inst, s);
#ifdef __USE_EX_FUNCTS__
      eF = _jalEx;
#else
      eF = _jal;
#endif
    }
  else
    {
      printf("Unknown JType instruction\n");
      exit(-1);
    }
  return eF;
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
exFunct getExecIType(uint32_t inst, state_t *s)
{
  uint32_t opcode = inst>>26;
  exFunct eF = 0;
  if(ITypeOpcodeTblEx[opcode] != 0)
    {
      eF = ITypeOpcodeTblEx[opcode];
      //ITypeOpcodeTbl[opcode](inst, s);
    }
  else
    {
      printf("%s: Unknown IType instruction (opcode=%x) @ pc=0x%08x\n", 
	     __func__, opcode, s ? s->pc : 0);
      exit(-1);
    }
  return eF;
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
      printf("%s: Unknown IType instruction (bits=%x) @ pc=0x%08x\n", 
	     __func__, inst, s ? s->pc : 0);
      exit(-1);
    }
}
exFunct getExecCoproc0(uint32_t inst, state_t *s)
{
  uint32_t opcode = inst>>26;
  uint32_t functField = (inst>>21) & 31;
  uint32_t sel = inst & 7;
  exFunct eF = 0;

  assert(sel==0);
  opcode &= 0x3;
 
  switch(functField)
    {
    case 0x0:
      /* move from coprocessor */
      eF = _mfc0;
      //_mfc0(inst,s);
      break;
    case 0x4:
      /* move to coprocessor */
      eF = _mtc0;
      //_mtc0(inst, s);
      break;
    default:
      printf("unknown %s instruction @ %x", __func__, s->pc);
      exit(-1);
      break;
    }
  return eF;
}
void execCoproc0(uint32_t inst, state_t *s)
{
  uint32_t opcode = inst>>26;
  uint32_t functField = (inst>>21) & 31;
  uint32_t sel = inst & 7;

  assert(sel==0);
  opcode &= 0x3;
 
  switch(functField)
    {
    case 0x0:
      /* move from coprocessor */
      _mfc0(inst,s);
      break;
    case 0x4:
      /* move to coprocessor */
      _mtc0(inst, s);
      break;
    default:
      printf("unknown %s instruction @ %x", __func__, s->pc);
      exit(-1);
      break;
    }
}
exFunct getExecCoproc1(uint32_t inst, state_t *s)
{
 uint32_t opcode = inst>>26;
 uint32_t functField = (inst>>21) & 31;
 uint32_t lowop = inst & 63;  
 uint32_t fmt = (inst >> 21) & 31;
  
 uint32_t rd = (inst>>11) & 31;
 uint32_t rt = (inst>>16) & 31;
 
 uint32_t fd = (inst>>6) & 31;
 uint32_t fs = (inst>>11) & 31;
 uint32_t nd_tf = (inst>>16) & 3;
 
 uint32_t lowbits = inst & ((1<<11)-1);
 opcode &= 0x3;
 exFunct eF = 0;
 if(fmt == 0x8)
   {
     switch(nd_tf)
       {
       case 0x0:
	 eF = _bc1fEx;
	 break;
       case 0x1:
	 eF = _bc1tEx;
	 break;
       case 0x2:
	 eF = _bc1flEx;
	 break;
	case 0x3:
	  eF = _bc1tlEx;
	  break;
       }
     /*BRANCH*/
   }
 else if((lowbits == 0) && ((functField==0x0) || (functField==0x4)))
   {
     if(functField == 0x0)
       {
	 /* move from coprocessor */
	 eF = _mfc1;
	}
     else if(functField == 0x4)
       {
	 /* move to coprocessor */
	 eF = _mtc1;
       }
   }
 else
   {
     if((lowop >> 4) == 3)
       {
	 eF = _c;
	}
     else
       {
	 switch(lowop)
	   {
	   case 0x0:
	     eF = _fadd;
	     break;
	   case 0x1:
	     eF =  _fsub;
	      break;
	   case 0x2:
	     eF = _fmul;
	     break;
	   case 0x3:
	      eF = _fdiv;
	      break;
	   case 0x4:
	     eF = _fsqrt;
	     break;
	   case 0x5:
	     eF = _fabs;
	     break;
	   case 0x6:
	     eF = _fmov;
	     break;
	   case 0x7:
	     eF = _fneg;
	     break;
	   case 0x9:
	     eF =_truncl;
	     break;
	   case 0xd:
	     eF = _truncw;
	     break;
	   case 0x11:
	     eF = _fmovc;
	     break;
	   case 0x12:
	     eF = _fmovz;
	     break;
	   case 0x13:
	     eF = _fmovn;
	     break;
	   case 0x15:
	     eF = _frecip;
	     break;
	   case 0x16:
	     eF = _frsqrt;
	     break;
	   case 0x20:
	     /* cvt.s */
	     eF = _cvts;
	     break;
	   case 0x21:
	     eF = _cvtd;
	     break;
	   default:
	     printf("unhandled coproc1 instruction (%x) @ %08x\n", inst, s->pc);
	     exit(-1);
	     break;
	   }
       }
   }
 return eF;
}

void execCoproc1(uint32_t inst, state_t *s)
{
  uint32_t opcode = inst>>26;
  uint32_t functField = (inst>>21) & 31;
  uint32_t lowop = inst & 63;  
  uint32_t fmt = (inst >> 21) & 31;
  
  uint32_t rd = (inst>>11) & 31;
  uint32_t rt = (inst>>16) & 31;

  uint32_t fd = (inst>>6) & 31;
  uint32_t fs = (inst>>11) & 31;
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

exFunct getExecCoproc2(uint32_t inst, state_t *s)
{
  printf("%s\n", __func__);
  exit(-1);
  return 0;
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
      if(cBB)
	cBB->print();
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
#ifdef USE_PHT
  cBB->setPrediction(0x3);
  branchPHT->update(s->pc, true);
#endif  
  uint32_t rs = (inst >> 21) & 31;
  uint32_t jaddr = s->gpr[rs];
  s->gpr[31] = s->pc+8;
  cBB->setTermAddr(s->pc);
  s->pc += 4;
  execMips(s);
  s->pc = jaddr;
  getNextBlock(s);
}

static void _jr(uint32_t inst, state_t *s)
{
#ifdef USE_PHT
  cBB->setPrediction(0x3);
  branchPHT->update(s->pc, true);
#endif  
  uint32_t rs = (inst >> 21) & 31;
  uint32_t jaddr = s->gpr[rs];
  
  cBB->setTermAddr(s->pc);

  s->pc += 4;
  execMips(s);
  s->pc = jaddr;
  getNextBlock(s);
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
  s->hi = s->gpr[rs];
  s->pc += 4;
}
static void _mtlo(uint32_t inst, state_t *s)
{
  uint32_t rs = (inst >> 21) & 31;
  s->lo = s->gpr[rs];
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

static void _maddu(uint32_t inst, state_t *s)
{
  uint64_t y,acc;
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;

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
  s->pc += 4;
}

static void _multu(uint32_t inst, state_t *s)
{
  uint64_t y;
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;

  uint32_t u0 = *((uint32_t*)&s->gpr[rs]);
  uint32_t u1 = *((uint32_t*)&s->gpr[rt]);
  uint64_t uk0 = (uint64_t)u0;
  uint64_t uk1 = (uint64_t)u1;
  y = uk0*uk1;
  *((uint32_t*)&(s->lo)) = (uint32_t)y;
  *((uint32_t*)&(s->hi)) = (uint32_t)(y>>32);
  s->pc += 4;
}

static void _mul(uint32_t inst, state_t *s)
{
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
    
  int64_t y = ((int64_t)s->gpr[rs]) * ((int64_t)s->gpr[rt]);
  s->gpr[rd] = (int32_t)y;
  s->pc += 4;
  
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
  uint32_t rs = (inst >> 21) & 31;
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  s->gpr[rd] = s->gpr[rt] >> s->gpr[rs];
  s->pc += 4;
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
  uint32_t y = u_rs - u_rt;
  s->gpr[rd] = y;
  s->pc += 4;
}
static void _syscall(uint32_t inst, state_t *s)
{ 
  printf("%s", __func__); exit(-1); 
}
static void _sync(uint32_t inst, state_t *s)
{
  /* this does nothing */
  s->pc += 4;
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
#ifdef USE_PHT
  branchPHT->update(s->pc, true);
#endif  
  cBB->setTermAddr(s->pc);
  uint32_t jaddr = inst & ((1<<26)-1);
  jaddr <<= 2;
  s->pc += 4;
  jaddr |= (s->pc & (~((1<<28)-1)));
  execMips(s);
  s->pc = jaddr;
  getNextBlock(s);
}
static void _jal(uint32_t inst, state_t *s)
{
#ifdef USE_PHT
  branchPHT->update(s->pc, true);
#endif  
  cBB->setTermAddr(s->pc);
  uint32_t jaddr = inst & ((1<<26)-1);
  jaddr <<= 2;
  s->gpr[31] = s->pc+8;
  s->pc += 4;
  jaddr |= (s->pc & (~((1<<28)-1)));
  execMips(s);
  s->pc = jaddr;
  getNextBlock(s);
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
#ifdef USE_PHT
  cBB->setPrediction(branchPHT->predict(s->pc));
  branchPHT->update(s->pc, takeBranch);
#endif  

  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
  cBB->setTermAddr(s->pc);
  uint32_t npc = s->pc+4; 

  /* execute branch delay */
  s->pc +=4;
  execMips(s);
  if(takeBranch)
    {
      s->pc = (imm+npc);
    }


  getNextBlock(s);
}

static void _beql(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  bool takeBranch = (s->gpr[rt] == s->gpr[rs]);
#ifdef USE_PHT
  cBB->setPrediction(branchPHT->predict(s->pc));
  branchPHT->update(s->pc, takeBranch);
#endif   
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
  cBB->setTermAddr(s->pc);
  cBB->setBranchLikely();
  uint32_t npc = s->pc+4; 
  s->pc +=4;

  /* execute branch delay */
  if(takeBranch)
    {
      execMips(s);
      s->pc = (imm+npc);
    }
  else
    {
      uint32_t bInst = accessBigEndian(*(uint32_t*)(s->mem + s->pc));
      cBB->addIns(bInst, s->pc);
      s->pc += 4;
    }

  getNextBlock(s);
}

static void _bne(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  bool takeBranch = (s->gpr[rt] != s->gpr[rs]);
#ifdef USE_PHT
  cBB->setPrediction(branchPHT->predict(s->pc));
  branchPHT->update(s->pc, takeBranch);
#endif  
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;

  cBB->setTermAddr(s->pc);
  uint32_t npc = s->pc+4; 
  
  /* execute branch delay */
  s->pc +=4;
  execMips(s);

  if(takeBranch)
    s->pc = (imm+npc);

  getNextBlock(s);
}



static void _bnel(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  bool takeBranch = (s->gpr[rt] != s->gpr[rs]);
#ifdef USE_PHT
  cBB->setPrediction(branchPHT->predict(s->pc));
  branchPHT->update(s->pc, takeBranch);
#endif  
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
  cBB->setTermAddr(s->pc);
  cBB->setBranchLikely();
  uint32_t npc = s->pc+4; 
  s->pc +=4;
  
  /* execute branch delay */
  if(takeBranch)
    {

      execMips(s);
      s->pc = (imm+npc);
    }
  else
    {
      uint32_t bInst = accessBigEndian(*(uint32_t*)(s->mem + s->pc));
      cBB->addIns(bInst, s->pc);
      s->pc += 4;
    }
  getNextBlock(s);
}

static void _bgtz(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
  int32_t npc = s->pc+4; 
  bool takeBranch = (s->gpr[rs]>0);
#ifdef USE_PHT
  cBB->setPrediction(branchPHT->predict(s->pc));
  branchPHT->update(s->pc, takeBranch);
#endif 
  cBB->setTermAddr(s->pc);
  s->pc += 4;
  execMips(s);
  if(takeBranch)
    s->pc = imm+npc;

  getNextBlock(s);
}

static void _bgtzl(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
  int32_t npc = s->pc+4; 
  bool takeBranch = (s->gpr[rs]>0);
#ifdef USE_PHT
  cBB->setPrediction(branchPHT->predict(s->pc));
  branchPHT->update(s->pc, takeBranch);
#endif  
  cBB->setTermAddr(s->pc);
  cBB->setBranchLikely();
  s->pc +=4;

  if(takeBranch)
    {
      execMips(s);
      s->pc = (imm+npc);
    }
  else
    {
      uint32_t bInst = accessBigEndian(*(uint32_t*)(s->mem + s->pc));
      cBB->addIns(bInst, s->pc);
      s->pc += 4;
    }
  getNextBlock(s);
}

static void _blezl(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
  int32_t npc = s->pc+4; 
  bool takeBranch = (s->gpr[rs]<=0);
#ifdef USE_PHT
  cBB->setPrediction(branchPHT->predict(s->pc));
  branchPHT->update(s->pc, takeBranch);
#endif  
  cBB->setTermAddr(s->pc);
  cBB->setBranchLikely();
  s->pc +=4;

  if(takeBranch)
    {
      execMips(s);
      s->pc = (imm+npc);
    }
  else
    {
      uint32_t bInst = accessBigEndian(*(uint32_t*)(s->mem + s->pc));
      cBB->addIns(bInst, s->pc);
      s->pc += 4;
    }
  getNextBlock(s);
}

static void _blez(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
  cBB->setTermAddr(s->pc);
  int32_t npc = s->pc+4; 
  bool takeBranch = (s->gpr[rs]<=0);
#ifdef USE_PHT
  cBB->setPrediction(branchPHT->predict(s->pc));
  branchPHT->update(s->pc, takeBranch);
#endif  
  s->pc += 4;
  execMips(s);
  if(takeBranch)
    s->pc = imm+npc;

  getNextBlock(s);
}

static void _bgez_bltz(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
#ifdef USE_PHT
  cBB->setPrediction(branchPHT->predict(s->pc));
#endif
  cBB->setTermAddr(s->pc);
  int32_t npc = s->pc+4; 
  bool takeBranch = false;
  if(rt >= 2) {
    cBB->setBranchLikely();
  }
  if(rt==0)
    {
      /* bltz : less than zero */
      takeBranch = (s->gpr[rs] < 0);
#ifdef USE_PHT
  branchPHT->update(s->pc, takeBranch);
#endif  
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
#ifdef USE_PHT
  branchPHT->update(s->pc, takeBranch);
#endif  
      s->pc += 4;
      execMips(s);
      if(takeBranch)
	s->pc = imm+npc;
      //s += "bgez " + regNames[rs] + "," + toStringHex(imm+npc);
    }
  else if(rt==2)
    {
      takeBranch = (s->gpr[rs] < 0);
#ifdef USE_PHT
  branchPHT->update(s->pc, takeBranch);
#endif  
      s->pc += 4;
      if(takeBranch)
	{
	  execMips(s);
	  s->pc = imm+npc;
	}
      else
	{
	  uint32_t bInst = accessBigEndian(*(uint32_t*)(s->mem + s->pc));
	  cBB->addIns(bInst, s->pc);
	  s->pc += 4;
	}
    }
  else if(rt == 3)
    {
      /* greater than zero likely */
      takeBranch = (s->gpr[rs] >=0);
#ifdef USE_PHT
  branchPHT->update(s->pc, takeBranch);
#endif  
      s->pc += 4;
      if(takeBranch)
	{
	  execMips(s);
	  s->pc = imm+npc;
	}
      else
	{
	  uint32_t bInst = accessBigEndian(*(uint32_t*)(s->mem + s->pc));
	  cBB->addIns(bInst, s->pc);
	  s->pc += 4;
	}
    }

  getNextBlock(s);

}

static void _lui(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t imm = inst & ((1<<16) - 1);
  imm <<= 16;
  s->gpr[rt] = imm;
  s->pc += 4;
}

static void _ll(uint32_t inst, state_t *s)
{
  /* uniprocessor implementation of ll */
  _lw(inst, s);
} 

static void _llSim(uint32_t inst, state_t *s)
{
  /* uniprocessor implementation of ll */
  _lwSim(inst, s);
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

static void _lwSim(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  uint32_t ea = (uint32_t)s->gpr[rs] + imm;

  s->gpr[rt] = accessBigEndian(*((int32_t*)(s->mem + ea))); 
  dCache->read(ea,4);
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

static void _lhSim(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  
  uint32_t ea = s->gpr[rs] + imm;
  int16_t mem = accessBigEndian(*((int16_t*)(s->mem + ea)));
  s->gpr[rt] = (int32_t)mem;
  dCache->read(ea,2);
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

static void _lbSim(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  
  uint32_t ea = s->gpr[rs] + imm;
  int8_t v = *((int8_t*)(s->mem + ea));
  s->gpr[rt] = (int32_t)v;
  dCache->read(ea,1);
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

static void _lbuSim(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  
  uint32_t ea = s->gpr[rs] + imm;
  uint32_t zExt = (uint32_t)s->mem[ea];
  *((uint32_t*)&(s->gpr[rt])) = zExt;
  dCache->read(ea,1);
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

static void _lhuSim(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  
  uint32_t ea = s->gpr[rs] + imm;
  uint32_t zExt = accessBigEndian(*((uint16_t*)(s->mem + ea)));
  *((uint32_t*)&(s->gpr[rt])) = zExt;
  dCache->read(ea,2);
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

static void _sc(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  _sw(inst, s);
  s->gpr[rt] = 1;
}

static void _scSim(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  _swSim(inst, s);
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

static void _swSim(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  uint32_t ea = s->gpr[rs] + imm;
  *((int32_t*)(s->mem + ea)) = accessBigEndian(s->gpr[rt]);
  dCache->write(ea,4);
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

static void _shSim(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
    
  uint32_t ea = s->gpr[rs] + imm;
  *((int16_t*)(s->mem + ea)) = accessBigEndian(((int16_t)s->gpr[rt]));
  dCache->write(ea,2);
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
static void _sbSim(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
    
  uint32_t ea = s->gpr[rs] + imm;
  s->mem[ea] = (uint8_t)s->gpr[rt];
  dCache->write(ea,1);
  s->pc +=4;
}

static void _seb(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rd = (inst >> 11) & 31;
  s->gpr[rd] = (int32_t)((int8_t)s->gpr[rt]);
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

static void _ins(uint32_t inst, state_t *s)
{
  //printf("%s\n", __func__);
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  uint32_t lsb = (inst >> 6) & 31;
  uint32_t msb = ((inst >> 11) & 31);
  uint32_t size = msb-lsb+1;
  uint32_t mask = (1U<<size) -1;
  uint32_t cmask = ~(mask << lsb);

  uint32_t v = (s->gpr[rs] & mask) << lsb;
  uint32_t c = (s->gpr[rt] & cmask) | v;
  /* store in rt */
  s->gpr[rt] = c;
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


static void _mtc0(uint32_t inst, state_t *s)
{
  uint32_t rd = (inst>>11) & 31;
  uint32_t rt = (inst>>16) & 31;
  s->cpr0[rd] = s->gpr[rt];
  s->pc += 4;
}

static void _mfc0(uint32_t inst, state_t *s)
{
  uint32_t rd = (inst>>11) & 31;
  uint32_t rt = (inst>>16) & 31;
  s->gpr[rt] = s->cpr0[rd];
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

void initState(state_t *s)
{

  memset(s, 0, sizeof(state_t));
  /* setup the status register */
  s->cpr0[12] |= 1<<2;
  s->cpr0[12] |= 1<<22;

}

uint8_t initPgState(state_t *s) 
{
  volatile uint8_t *v_mem = s->mem;
  uint8_t p = 0;
  for(size_t pg = 0; pg < (1<<20); pg++) {
    /* touch each page to ensure that's resident */
    p ^= v_mem[4096*pg];
    /* mark all pages as initially no read/no write */
    s->pgstate[pg] = PROT_NONE;
    /* mprotect will fail if it's not resident */
    int rc = mprotect((void*)(v_mem + 4096*pg), 4096, PROT_NONE);
    if( rc < 0) {
      char *err = strerror(errno);
      printf("mprotect failed..%s\n",err);
      exit(-1);
    }  
  }
  return p;
}


void markReadWritePagesAsReadOnly(state_t *s) 
{
  for(size_t pg = 0; pg < (1<<20); pg++) {
    if(s->pgstate[pg] == (PROT_READ|PROT_WRITE)) {
      int rc = mprotect((void*)(s->mem + 4096*pg), 4096, PROT_READ);
      if(rc < 0) {
	char *err = strerror(errno);
	printf("mprotect failed..%s\n",err);
	exit(-1);
      }
      s->pgstate[pg] = PROT_READ;
    }
  }

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
  cBB->setTermAddr(s->pc);
  _monitorBody(inst, s);
  getNextBlock(s);
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
  
  uint32_t r = accessBigEndian(*((int32_t*)(s->mem + ea))); 
  uint32_t xx=0,x = s->gpr[rt];
  
  uint32_t xs = x >> (8*ma);
  uint32_t m = ~((1U << (8*(4 - ma))) - 1);
  xx = (r & m) | xs;
  *((uint32_t*)(s->mem + ea)) = accessBigEndian(xx);
  s->pc += 4;
}

static void _swlSim(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;

  uint32_t ea = s->gpr[rs] + imm;
  uint32_t ma = ea & 3;
  ea &= 0xfffffffc;
  
  uint32_t r = accessBigEndian(*((int32_t*)(s->mem + ea))); 
  uint32_t xx=0,x = s->gpr[rt];
  
  uint32_t xs = x >> (8*ma);
  uint32_t m = ~((1U << (8*(4 - ma))) - 1);
  xx = (r & m) | xs;
  *((uint32_t*)(s->mem + ea)) = accessBigEndian(xx);
  dCache->read(ea, 4);
  dCache->write(ea, 4);
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
  ea &= 0xfffffffc;
  uint32_t r = accessBigEndian(*((int32_t*)(s->mem + ea))); 
  uint32_t xx=0,x = s->gpr[rt];
  
  uint32_t xs = 8*(3-ma);
  uint32_t rm = (1U << xs) - 1;

  xx = (x << xs) | (rm & r);
  *((uint32_t*)(s->mem + ea)) = accessBigEndian(xx);

  s->pc += 4;
}

static void _swrSim(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
   
  uint32_t ea = s->gpr[rs] + imm;
  uint32_t ma = ea & 3;
  ea &= 0xfffffffc;
  uint32_t r = accessBigEndian(*((int32_t*)(s->mem + ea))); 
  uint32_t xx=0,x = s->gpr[rt];
  
  uint32_t xs = 8*(3-ma);
  uint32_t rm = (1U << xs) - 1;

  xx = (x << xs) | (rm & r);
  *((uint32_t*)(s->mem + ea)) = accessBigEndian(xx);
  dCache->read(ea, 4);
  dCache->write(ea, 4);
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

static void _lwlSim(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  
  uint32_t ea = ((uint32_t)s->gpr[rs] + imm);
  uint32_t ma = ea & 3;
  ea &= 0xfffffffc;

  int32_t r = accessBigEndian(*((int32_t*)(s->mem + ea))); 
  dCache->read(ea, 4);
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


static void _lwrSim(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
 
  uint32_t ea = ((uint32_t)s->gpr[rs] + imm);
  dCache->read(ea, 4);

  uint32_t ma = ea & 3;
  ea &= 0xfffffffc;
  
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


static void _jalrEx(uint32_t inst, state_t *s)
{
#ifdef USE_PHT
  cBB->setPrediction(0x3);
  branchPHT->update(s->pc, true);
#endif  
  uint32_t rs = (inst >> 21) & 31;
  uint32_t jaddr = s->gpr[rs];
  s->gpr[31] = s->pc+8;
  s->pc += 4;
  execMips(s);
  s->pc = jaddr;
}
static void _jrEx(uint32_t inst, state_t *s)
{
#ifdef USE_PHT
  cBB->setPrediction(0x3);
  branchPHT->update(s->pc, true);
#endif  
  uint32_t rs = (inst >> 21) & 31;
  uint32_t jaddr = s->gpr[rs];
    
  s->pc += 4;
  execMips(s);
  s->pc = jaddr;
}
static void _jEx(uint32_t inst, state_t *s)
{
#ifdef USE_PHT
  cBB->setPrediction(0x3);
  branchPHT->update(s->pc, true);
#endif  
  uint32_t jaddr = inst & ((1<<26)-1);
  jaddr <<= 2;
  s->pc += 4;
  jaddr |= (s->pc & (~((1<<28)-1)));
  execMips(s);
  s->pc = jaddr;
}
static void _jalEx(uint32_t inst, state_t *s)
{
#ifdef USE_PHT
  cBB->setPrediction(0x3);
  branchPHT->update(s->pc, true);
#endif  
  uint32_t jaddr = inst & ((1<<26)-1);
  jaddr <<= 2;
  s->gpr[31] = s->pc+8;
  s->pc += 4;
  jaddr |= (s->pc & (~((1<<28)-1)));
  execMips(s);
  s->pc = jaddr;
}
static void _beqEx(uint32_t inst, state_t *s)
{
 uint32_t rt = (inst >> 16) & 31;
 uint32_t rs = (inst >> 21) & 31;
 bool takeBranch = (s->gpr[rt] == s->gpr[rs]);
#ifdef USE_PHT
  cBB->setPrediction(branchPHT->predict(s->pc));
  branchPHT->update(s->pc, takeBranch);
#endif  
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
static void _beqlEx(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  bool takeBranch = (s->gpr[rt] == s->gpr[rs]);
#ifdef USE_PHT
  cBB->setPrediction(branchPHT->predict(s->pc));
  branchPHT->update(s->pc, takeBranch);
#endif  
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

static void _bneEx(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  bool takeBranch = (s->gpr[rt] != s->gpr[rs]);
#ifdef USE_PHT
  cBB->setPrediction(branchPHT->predict(s->pc));
  branchPHT->update(s->pc, takeBranch);
#endif  
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
  uint32_t npc = s->pc+4; 
  
  /* execute branch delay */
  s->pc +=4;
  execMips(s);

  if(takeBranch)
    s->pc = (imm+npc);
}
static void _bnelEx(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  bool takeBranch = (s->gpr[rt] != s->gpr[rs]);
#ifdef USE_PHT
  cBB->setPrediction(branchPHT->predict(s->pc));
  branchPHT->update(s->pc, takeBranch);
#endif  
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

static void _bgtzEx(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
  int32_t npc = s->pc+4; 
  bool takeBranch = (s->gpr[rs]>0);
#ifdef USE_PHT
  cBB->setPrediction(branchPHT->predict(s->pc));
  branchPHT->update(s->pc, takeBranch);
#endif  
  s->pc += 4;
  execMips(s);
  if(takeBranch)
    s->pc = imm+npc;
}

static void _bgtzlEx(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
  int32_t npc = s->pc+4; 
  bool takeBranch = (s->gpr[rs]>0);
#ifdef USE_PHT
  cBB->setPrediction(branchPHT->predict(s->pc));
  branchPHT->update(s->pc, takeBranch);
#endif  
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
static void _blezlEx(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
  int32_t npc = s->pc+4; 
  bool takeBranch = (s->gpr[rs]<=0);
#ifdef USE_PHT
  cBB->setPrediction(branchPHT->predict(s->pc));
  branchPHT->update(s->pc, takeBranch);
#endif  
  s->pc += 4;

  if(takeBranch)
    {
      execMips(s);
      s->pc = imm+npc;
    }
  else
    {
      s->pc += 4;
    }
}

static void _blezEx(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
  int32_t npc = s->pc+4; 
  bool takeBranch = (s->gpr[rs]<=0);
#ifdef USE_PHT
  branchPHT->update(s->pc, takeBranch);
#endif  
  s->pc += 4;
  execMips(s);
  if(takeBranch)
    s->pc = imm+npc;
}


static void _bgez_bltzEx(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
  int32_t npc = s->pc+4; 
  bool takeBranch = false;
#ifdef USE_PHT
  cBB->setPrediction(branchPHT->predict(s->pc));
#endif
  if(rt==0)
    {
      /* bltz : less than zero */
      takeBranch = (s->gpr[rs] < 0);
#ifdef USE_PHT
      branchPHT->update(s->pc, takeBranch);
#endif  
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
#ifdef USE_PHT
  branchPHT->update(s->pc, takeBranch);
#endif  
      s->pc += 4;
      execMips(s);
      if(takeBranch)
	s->pc = imm+npc;
      //s += "bgez " + regNames[rs] + "," + toStringHex(imm+npc);
    }
  else if(rt==2)
    {
      takeBranch = (s->gpr[rs] < 0);
#ifdef USE_PHT
  branchPHT->update(s->pc, takeBranch);
#endif  
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
#ifdef USE_PHT
  branchPHT->update(s->pc, takeBranch);
#endif  
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

static void _monitorBody(uint32_t inst, state_t *s)
{
 uint32_t reason = (inst >> RSVD_INSTRUCTION_ARG_SHIFT) & 
    RSVD_INSTRUCTION_ARG_MASK;
  reason >>= 1;
  int32_t fd=-1,nr=-1,flags=-1;
  char *path;
  struct timeval tp;
  timeval32_t tp32;
  struct tms tms_buf;
  tms32_t tms32_buf;
  struct stat native_stat;
  stat32_t *host_stat = NULL;

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
      /* printf("fd = %d, nr = %d\n", fd, nr); */
      s->gpr[R_v0] = (int32_t)write(fd, (void*)(s->mem + (uint32_t)s->gpr[R_a1]), nr);
      if(fd==1)
	fflush(stdout);
      else if(fd==2)
	fflush(stderr);
      //printf("%s", (char*)(s->mem + (uint32_t)s->gpr[R_a1]));
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
      if(enTimingFuncts) {
	gettimeofday(&tp, NULL);
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
      if(enTimingFuncts) {
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
      //printf("returning %d\n", s->gpr[R_v0]);
      *((tms32_t*)(s->mem + (uint32_t)s->gpr[R_a0] + 0)) = tms32_buf;
      break;
    case 35:
      /* int getargs(char **argv) */
      for(int i = 0; i < std::min(MARGS, sArgc); i++)
	{
	  uint32_t arrayAddr = ((uint32_t)s->gpr[R_a0])+4*i;
	  uint32_t ptr = accessBigEndian(*((uint32_t*)(s->mem + arrayAddr)));
	  strcpy((char*)(s->mem + ptr), sArgv[i]);
	}
      s->gpr[R_v0] = sArgc;
      break;
    case 36:
      //if(!syncAndHash((void*)s, (void*)&otherState ,424)) {
      //printf("aborting with x = %d\n", s->gpr[R_a0]);
      //if(server) {
      //  compareState(s, &otherState);
      //}
      //}
      abort();
      break;
    case 37:
      /*char *getcwd(char *buf, uint32_t size) */
      path = (char*)(s->mem + (uint32_t)s->gpr[R_a0]);
      path = getcwd(path, (uint32_t)s->gpr[R_a1]);
      s->gpr[R_v0] = s->gpr[R_a0];
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

static void _monitorEx(uint32_t inst, state_t *s)
{
  _monitorBody(inst, s);
}

static void getNextBlock(state_t *s)
{
  basicBlock *nBB = cBB->findBlock(s->pc);
  if(nBB == 0) {
    nBB = new basicBlock(s->pc, cBB);
  }
  cBB->setReadOnly();
  cBB = nBB;
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
static void _ldc1Sim(uint32_t inst, state_t *s)
{
  uint32_t ft = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  uint32_t ea = s->gpr[rs] + imm;
  *((int64_t*)(s->cpr1 + ft)) = accessBigEndian(*((int64_t*)(s->mem + ea))); 
  dCache->read(ea,8);
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
static void _sdc1Sim(uint32_t inst, state_t *s)
{
  uint32_t ft = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  uint32_t ea = s->gpr[rs] + imm;
  *((int64_t*)(s->mem + ea)) = accessBigEndian((*(int64_t*)(s->cpr1 + ft)));
  dCache->write(ea,8);
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
static void _lwc1Sim(uint32_t inst, state_t *s)
{
  uint32_t ft = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  uint32_t ea = s->gpr[rs] + imm;
  uint32_t v = accessBigEndian(*((uint32_t*)(s->mem + ea))); 
  *((float*)(s->cpr1 + ft)) = *((float*)&v);
  dCache->read(ea,4);
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
static void _swc1Sim(uint32_t inst, state_t *s)
{
  uint32_t ft = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = (int32_t)himm;
  uint32_t ea = s->gpr[rs] + imm;
  uint32_t v = *((uint32_t*)(s->cpr1+ft));
  *((uint32_t*)(s->mem + ea)) = accessBigEndian(v);
  dCache->write(ea,4);
  s->pc += 4;
}
static void _bc1fEx(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
  int32_t npc = s->pc+4; 
  uint32_t cc = (inst >> 18) & 7;
  bool takeBranch = getConditionCode(s,cc)==0;
#ifdef USE_PHT
  cBB->setPrediction(branchPHT->predict(s->pc));
  branchPHT->update(s->pc, takeBranch);
#endif  
  s->pc += 4;
  execMips(s);
  if(takeBranch)
    {
      s->pc = imm+npc;
    }
}
static void _bc1tEx(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
  int32_t npc = s->pc+4; 
  uint32_t cc = (inst >> 18) & 7;
  bool takeBranch = getConditionCode(s,cc)==1;
#ifdef USE_PHT
  cBB->setPrediction(branchPHT->predict(s->pc));
  branchPHT->update(s->pc, takeBranch);
#endif  
  //printf("%s @ s->pc = %x, npc = %x, icnt =%lu\n",
  //	 __func__, s->pc, imm+npc, s->icnt);
  s->pc += 4;
  execMips(s);
  
  if(takeBranch)
    {
      s->pc = (imm+npc);
    }
}

static void _bc1flEx(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
  int32_t npc = s->pc+4; 
  uint32_t cc = (inst >> 18) & 7;
  bool takeBranch = getConditionCode(s,cc)==0;
#ifdef USE_PHT
  cBB->setPrediction(branchPHT->predict(s->pc));
  branchPHT->update(s->pc, takeBranch);
#endif  
  //printf("%s @ s->pc = %x, npc = %x, icnt =%lu\n",
  // __func__, s->pc, imm+npc, s->icnt);
  
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

static void _bc1tlEx(uint32_t inst, state_t *s)
{
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
  int32_t npc = s->pc+4; 
  uint32_t cc = (inst >> 18) & 7;
  bool takeBranch = getConditionCode(s,cc)==1;
#ifdef USE_PHT
  cBB->setPrediction(branchPHT->predict(s->pc));
  branchPHT->update(s->pc, takeBranch);
#endif  
  //printf("%s @ s->pc = %x, npc = %x, icnt =%lu\n",
  //	 __func__, s->pc, imm+npc, s->icnt);
  
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


/* normal versions */
static void _bc1f(uint32_t inst, state_t *s)
{
#ifdef USE_PHT
  cBB->setPrediction(branchPHT->predict(s->pc));
#endif

  cBB->setTermAddr(s->pc);
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
  int32_t npc = s->pc+4; 
  uint32_t cc = (inst >> 18) & 7;
  bool takeBranch = getConditionCode(s,cc)==0;

#ifdef USE_PHT
  branchPHT->update(s->pc, takeBranch);
#endif  
  //printf("%s @ s->pc = %x, npc = %x, icnt =%lu\n",
  //	 __func__, s->pc, imm+npc, s->icnt);
  s->pc += 4;
  execMips(s);
  if(takeBranch)
    {
      s->pc = imm+npc;
    }
  getNextBlock(s);
}
static void _bc1t(uint32_t inst, state_t *s)
{ 
#ifdef USE_PHT 
  cBB->setPrediction(branchPHT->predict(s->pc));
#endif
  cBB->setTermAddr(s->pc);
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
  int32_t npc = s->pc+4; 
  uint32_t cc = (inst >> 18) & 7;
  bool takeBranch = getConditionCode(s,cc)==1;
#ifdef USE_PHT
  branchPHT->update(s->pc, takeBranch);
#endif  
  //printf("%s @ s->pc = %x, npc = %x, icnt =%lu\n",
  //__func__, s->pc, imm+npc, s->icnt);
  s->pc += 4;
  execMips(s);
  
  if(takeBranch)
    {
      s->pc = (imm+npc);
    }
  getNextBlock(s);
}

static void _bc1fl(uint32_t inst, state_t *s)
{
#ifdef USE_PHT
  cBB->setPrediction(branchPHT->predict(s->pc));
#endif
  cBB->setTermAddr(s->pc);
  cBB->setBranchLikely();
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
  int32_t npc = s->pc+4; 
  uint32_t cc = (inst >> 18) & 7;
  bool takeBranch = getConditionCode(s,cc)==0;
#ifdef USE_PHT
  branchPHT->update(s->pc, takeBranch);
#endif  
  s->pc +=4;

  if(takeBranch)
    {
      execMips(s);
      s->pc = (imm+npc);
    }
  else
    {
      uint32_t bInst = accessBigEndian(*(uint32_t*)(s->mem + s->pc));
      cBB->addIns(bInst, s->pc);
      s->pc += 4;
    }
  getNextBlock(s);
}

static void _bc1tl(uint32_t inst, state_t *s)
{
#ifdef USE_PHT
  cBB->setPrediction(branchPHT->predict(s->pc));
#endif
  cBB->setTermAddr(s->pc);
  cBB->setBranchLikely();
  uint32_t rt = (inst >> 16) & 31;
  uint32_t rs = (inst >> 21) & 31;
  int16_t himm = (int16_t)(inst & ((1<<16) - 1));
  int32_t imm = ((int32_t)himm) << 2;
  int32_t npc = s->pc+4; 
  uint32_t cc = (inst >> 18) & 7;
  bool takeBranch = getConditionCode(s,cc)==1;
#ifdef USE_PHT
  branchPHT->update(s->pc, takeBranch);
#endif  
  s->pc +=4;

  if(takeBranch)
    {
      
      execMips(s);
      s->pc = (imm+npc);
    }
  else
    {
      uint32_t bInst = accessBigEndian(*(uint32_t*)(s->mem + s->pc));
      cBB->addIns(bInst, s->pc);
      s->pc += 4;
    }
  getNextBlock(s);
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

static void _truncl(uint32_t inst, state_t *s)
{
  printf("%s\n",__func__);
  exit(-1);
}
static void _movd(uint32_t inst, state_t *s)
{
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
