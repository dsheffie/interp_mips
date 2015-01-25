#ifndef __GLOBALS_H__
#define __GLOBALS_H__

#define ENABLE_LOGGING 1

extern std::vector<uint64_t> instAbortCounts;
extern bool enComplDouble;
extern bool enComplSingle;

extern basicBlock *cBB;
extern simCache *dCache;

extern size_t bbHotThresh;

extern uint64_t traceCycles;
extern uint64_t jitCycles;
extern uint64_t interpCycles;
extern size_t findBlockCycles;
extern size_t updateLinksCycles;

extern size_t numInstBlock;
extern size_t numInstTrace;

extern uint64_t traceCompileCycles;
extern uint64_t blockCompileCycles;

extern std::string *executeLog;

extern std::map<uint32_t, uint8_t*> dirtyPageMap;

#endif
