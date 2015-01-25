#include <string>
#include <stdint.h>

#ifndef __PARSE_MIPS__
#define __PARSE_MIPS__

#define R_zero 0
#define R_at 1
#define R_v0 2
#define R_v1 3
#define R_a0 4
#define R_a1 5
#define R_a2 6
#define R_a3 7
#define R_t0 8
#define R_t1 9
#define R_t2 10
#define R_t3 11
#define R_t4 12
#define R_t5 13
#define R_t6 14
#define R_t7 15
#define R_s0 16
#define R_s1 17
#define R_s2 18
#define R_s3 19
#define R_s4 20
#define R_s5 21
#define R_s6 22
#define R_s7 23
#define R_t8 24
#define R_t9 25
#define R_k0 26
#define R_k1 27
#define R_gp 28
#define R_sp 29
#define R_s8 30
#define R_ra 31

#define FMT_S 16 /* single precision */
#define FMT_D 17 /* double precision */
#define FMT_E 18 /* extended precision */
#define FMT_Q 19 /* quad precision */
#define FMT_W 20 /* 32-bit fixed */
#define FMT_L 21 /* 64-bit fixed */

#define COND_F 0
#define COND_UN 1
#define COND_EQ 2
#define COND_UEQ 3
#define COND_OLT 4
#define COND_ULT 5
#define COND_OLE 6
#define COND_ULE 7
#define COND_SF 8
#define COND_NGLE 9
#define COND_SEQ 10
#define COND_NGL 11
#define COND_LT 12
#define COND_NGE 13
#define COND_LE 14
#define COND_NGT 15

#define CP1_CR0 0
#define CP1_CR31 1
#define CP1_CR25 2
#define CP1_CR26 3
#define CP1_CR28 4

enum MipsInst_t {
  inst_monitor=0, 
  inst_add, 
  inst_addu, 
  inst_and,
  inst_break,
  inst_div,
  inst_divu,
  inst_jalr,
  inst_jr,
  inst_movn,
  inst_movz,
  inst_mfhi,
  inst_mflo,
  inst_mthi,
  inst_mtlo,
  inst_mult,
  inst_multu,
  inst_madd,
  inst_maddu,
  inst_mul,
  inst_nor,
  inst_or,
  inst_sll,
  inst_sllv,
  inst_slt,
  inst_sltu,
  inst_sra,
  inst_srav, 
  inst_srl,
  inst_srlv,
  inst_sub,
  inst_subu,
  inst_syscall,
  inst_xor,
  inst_tge,
  inst_teq,
  inst_j,
  inst_jal,
  inst_addi,
  inst_addiu,
  inst_andi,
  inst_ori,
  inst_xori,
  inst_beq,
  inst_beql,
  inst_bne,
  inst_bnel,
  inst_bgtzl,
  inst_bgtz,
  inst_blez,
  inst_blezl,
  inst_bgez_bltz,
  inst_lui,
  inst_lh,
  inst_lb,
  inst_lbu,
  inst_lhu,
  inst_slti,
  inst_sltiu,
  inst_sw,
  inst_sh,
  inst_sb,
  inst_lwl,
  inst_lw,
  inst_ext,
  inst_seh,
  inst_ins,
  inst_clz,
  inst_swl,
  inst_swr,
  inst_sdc1,
  inst_ldc1,
  inst_bc1f,
  inst_bc1t,
  inst_bc1fl,
  inst_bc1tl,
  inst_lwc1,
  inst_swc1,
  inst_movci,
  inst_mfc1,
  inst_mtc1,
  inst_cvtd,
  inst_cvts,
  inst_truncl,
  inst_truncw,
  inst_lwr,
  inst_seb,
  inst_mfc0,
  inst_mtc0,
  inst_unknown
};

void initParseTables();
bool isBranchOrJump(uint32_t inst);
std::string getAsmString(uint32_t inst,uint32_t addr);
std::string getGPRName(uint32_t r, bool spaces);
std::string getCondName(uint32_t c);

MipsInst_t getInstType(uint32_t inst);
std::string getInstTypeStr(uint32_t idx);
#endif
