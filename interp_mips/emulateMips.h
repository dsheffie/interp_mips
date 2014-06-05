#ifndef __EMULATEMIPS__
#define __EMULATEMIPS__

#include <stdint.h>

/* from gdb simulator */
#define RSVD_INSTRUCTION           (0x00000005)
#define RSVD_INSTRUCTION_MASK      (0xFC00003F)
#define RSVD_INSTRUCTION_ARG_SHIFT 6
#define RSVD_INSTRUCTION_ARG_MASK  0xFFFFF  
#define IDT_MONITOR_BASE           0xBFC00000
#define IDT_MONITOR_SIZE           2048


typedef struct
{
  uint32_t pc;
  int32_t gpr[32];
  uint32_t cpr0[32];
  uint64_t cpr1[32];
  uint32_t fcr1[5];
  int32_t lo;
  int32_t hi;
  uint8_t *mem;
  uint8_t brk;
  uint64_t icnt;
} state_t;

void initEmulationTables(bool enClockFuncts);
void initState(state_t *s);
void execMips(state_t *s);
void printState(state_t *s);

uint32_t getConditionCode(state_t *s, uint32_t cc);
void setConditionCode(state_t *s, uint32_t v, uint32_t cc);

void mkMonitorVectors(state_t *s);

#endif
