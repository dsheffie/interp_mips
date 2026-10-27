#ifndef __GLOBALSH__
#define __GLOBALSH__

#include <cstdio>
#include <cstdint>
#include <unordered_map>

class retire_trace;

namespace globals {
  extern bool enClockFuncts;
  extern uint64_t icountMIPS;
  extern uint64_t cycle;
  extern bool trace_retirement;
  extern bool trace_fp;
  extern bool report_syscalls;
  /* PCTRACEOUT co-sim trace: when pctrace != null, each retired instruction
   * (delay slots included) appends its 32-bit virtual PC, once execution first
   * reaches pctrace_start. */
  extern FILE *pctrace;
  extern uint32_t pctrace_start;
  extern bool pctrace_on;
  extern bool asidpc_armed;
  /* When non-null and trace_retirement is set, each retired instruction is
   * appended as an inst_record{pc=physical, vpc=virtual, inst} for the
   * rv64analyzer trace consumer. */
  extern retire_trace *retire_log;
  /* --mempcs/--memout (phase 2 of mips-analyzer's memory analysis): for each
   * load/store/pref/cache whose virtual pc is in mem_stats, the address range
   * it touched, plus its first inter-instance stride and how many deltas broke
   * that stride.  ea comes from the registers BEFORE the op runs (lw $t0,0($t0)
   * overwrites its base).  Written to --memout at exit. */
  struct mem_stat_t {
    uint64_t count = 0, lo = ~0UL, hi = 0, last = 0, breaks = 0;
    int64_t stride = 0;
    uint32_t bytes = 0;
  };
  extern std::unordered_map<uint64_t, mem_stat_t> *mem_stats;
  /* --cpu r5000: present an R5000PC Indy (PRId/FIR imp 0x23, 32K+32K L1, no L2)
   * and enable the MIPS IV instructions.  Default (false) is the R4400 / MIPS III
   * machine r9999 implements: MIPS IV ops raise RI (or FP Unimplemented). */
  extern bool cpu_r5000;
};

#endif
