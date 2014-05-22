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

void initParseTables();

std::string getAsmString(uint32_t inst,uint32_t addr);
std::string getGPRName(uint32_t r);
#endif
