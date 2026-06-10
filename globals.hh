#ifndef __GLOBALSH__
#define __GLOBALSH__

class retire_trace;

namespace globals {
  extern bool enClockFuncts;
  extern uint64_t icountMIPS;
  extern uint64_t cycle;
  extern bool trace_retirement;
  extern bool trace_fp;
  extern bool report_syscalls;
  /* When non-null and trace_retirement is set, each retired instruction is
   * appended as an inst_record{pc=physical, vpc=virtual, inst} for the
   * rv64analyzer trace consumer. */
  extern retire_trace *retire_log;
};

#endif
