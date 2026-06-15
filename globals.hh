#ifndef __GLOBALSH__
#define __GLOBALSH__

#include <cstdio>
#include <cstdint>

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
  /* When non-null and trace_retirement is set, each retired instruction is
   * appended as an inst_record{pc=physical, vpc=virtual, inst} for the
   * rv64analyzer trace consumer. */
  extern retire_trace *retire_log;
};

#endif
