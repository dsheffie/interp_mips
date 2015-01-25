#ifndef __SIM_BASICBLOCK__
#define __SIM_BASICBLOCK__

#include <set>
#include <vector>
#include <map>
#include <string>
#include <stdint.h>
#include <cstdlib>
#include "profileMips.hh"

#include <google/dense_hash_map>


class basicBlock
{
 public:
  static uint64_t cfgCnt;
  uint32_t bPred;
  bool canCompile;
  bool isCompiled;
  bool hasTrace;
  std::vector<std::vector<basicBlock*> >bbTraces;
  uint32_t traceCRC;
  bool useJIT;
  bool useTraceJIT;
  bool hasTermBranchOrJump;
  friend class graph;
    
  /* heads of traces that include this block */
  std::set<basicBlock*> cfgInTraces;

  std::set<basicBlock *> preds;
  std::set<basicBlock *> succs;
  google::dense_hash_map<uint32_t, basicBlock *> succsMap;
  
  uint32_t entryAddr;
  uint32_t termAddr;
  uint64_t cnt;
  ssize_t length;
  size_t traceAbortCnt;

  bool readOnly;
  bool branchLikely;
  bool hasBeenSplit;
  std::vector<std::pair<uint32_t, uint32_t> > vecIns;
  std::vector<exFunct> insFuncts;

  std::map<uint32_t, basicBlock*> *bbMap;
  std::map<uint32_t, basicBlock*> *insMap;

  basicBlock* split(uint32_t nEntryAddr);
  void run(state_t *s);
  /* time spent in emulation */
  double emulatedTime;
  /* time spent in binary translated code */
  double binaryTime;

  void setPrediction(uint32_t p) {
    bPred = p;
  }
  bool blockCompiled() {
    return isCompiled;
  }
  bool traceCompiled() {
    return hasTrace;
  }
  bool hasSelfLoop();
  uint32_t getEntryAddr()
  {
    return entryAddr;
  }
  void setReadOnly();
  void setBranchLikely() {
    branchLikely = true;
  }
  bool hasBranchLikely() {
    return branchLikely;
  }
  void setTermAddr( uint32_t termAddr); 

  void print();
  void fprint(FILE *fp);
  void addIns(uint32_t inst, uint32_t addr);
  basicBlock(uint32_t entryAddr, basicBlock *prev);
  
  basicBlock(uint32_t entryAddr, 
	     bool useJIT,
	     bool useTraceJIT,
	     std::map<uint32_t, basicBlock*> *insMap,
	     std::map<uint32_t, basicBlock*> *bbMap);

  basicBlock *findBlock(uint32_t entryAddr);
  void makeDot(std::string &fname);
  bool execute(state_t *s);
  ~basicBlock();
};

#endif
