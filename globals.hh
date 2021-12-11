#ifndef __GLOBALSH__
#define __GLOBALSH__

namespace globals {
  extern bool enClockFuncts;
  extern int sysArgc;
  extern char **sysArgv;
  extern bool isMipsEL;
  extern uint64_t icountMIPS;
  extern bool silent;
  extern std::map<uint32_t, uint64_t> execHisto;
};

#endif
