#ifndef __PROFILEMIPS__
#define __PROFILEMIPS__

#include <stdint.h>
#include <cstdlib>
#include <cstdio>

/* from gdb simulator */
#define RSVD_INSTRUCTION           (0x00000005)
#define RSVD_INSTRUCTION_MASK      (0xFC00003F)
#define RSVD_INSTRUCTION_ARG_SHIFT 6
#define RSVD_INSTRUCTION_ARG_MASK  0xFFFFF  
#define IDT_MONITOR_BASE           0xBFC00000
#define IDT_MONITOR_SIZE           2048
#define MARGS 20
#define ENTRY_POINT 0xa0020000

typedef struct
{
  uint32_t pc;
  int32_t gpr[32];
  int32_t lo;
  int32_t hi;
  uint32_t cpr0[32];
  uint32_t cpr1[32];
  uint32_t fcr1[5];
  uint64_t icnt;
  uint8_t *mem;
  int *pgstate;
  uint8_t brk;
  uint32_t abortloc;
  uint32_t oldpc;
  uint8_t mode;
} state_t;

void initEmulationTables(bool enClockFuncts, int sysArgc, char **sysArgv);
void initState(state_t *s);
uint8_t initPgState(state_t *s);
void markReadWritePagesAsReadOnly(state_t *s);
void execMips(state_t *s);
void printState(state_t *s);
void hashState(state_t *s);
void hashState(state_t *s, FILE *fp);

void mkMonitorVectors(state_t *s);

typedef void (*exFunct)(uint32_t inst, state_t *s);
exFunct decForExec(uint32_t inst, state_t *s);
bool getStaticBranchTarget(uint32_t inst, uint32_t pc, uint32_t &bTarget);
void compareState(state_t *s0, state_t *s1);
#endif
