#include "basicBlock.hh"
#include "parseMips.hh"
#include "helper.hh"
#include "simCache.hh"

#include <string>
#include <cstdlib>
#include <cstdio>

#include <execinfo.h>
#include <cxxabi.h>
#include <unistd.h>
#include <sys/mman.h>

#include "globals.hh"

uint64_t basicBlock::cfgCnt = 0;

void basicBlock::setReadOnly()
{
  if(!readOnly)
    {
      length = (termAddr-entryAddr)/4;
      readOnly = true;
      insFuncts.clear();
      for(size_t i = 0; i < vecIns.size(); i++)
	{
	  exFunct foo = decForExec(vecIns[i].first, NULL);
	  insFuncts.push_back(foo);
	}
      
      canCompile = false;
      isCompiled = false;
      hasTrace = false;
      bbTraces.clear();
    }
 }

basicBlock::basicBlock(uint32_t entryAddr, basicBlock *prev)
{
  traceCRC = 0x0;
  bPred = 0xffffffff;
  succsMap.set_empty_key(0);
  length = -1;
  traceAbortCnt = 0;
  canCompile = false;
  isCompiled = false;
  hasTermBranchOrJump = false;
  emulatedTime = 0.0;
  binaryTime = 0.0;
  hasTrace = false;
  useJIT = prev->useJIT;
  useTraceJIT = prev->useTraceJIT;
  this->entryAddr = entryAddr;
  termAddr = 0;
  preds.insert(prev);
  cnt = 0;
  hasBeenSplit = false;
  branchLikely = false;
  readOnly = false;
  
  bbMap = prev->bbMap;
  insMap = prev->insMap;
  prev->succs.insert(this);
  prev->succsMap[entryAddr] = this;
  (*bbMap)[entryAddr] = this;
}

basicBlock::basicBlock(uint32_t entryAddr, 
		       bool useJIT,
		       bool useTraceJIT,
		       std::map<uint32_t, basicBlock*> *insMap, 
		       std::map<uint32_t, basicBlock*> *bbMap)
{
  traceCRC = 0x0;
  bPred = 0xffffffff;
  succsMap.set_empty_key(0);
  length = -1;
  canCompile = false;
  isCompiled = false;
  hasTrace = false;
  traceAbortCnt = 0;
  hasTermBranchOrJump = false;
  this->useJIT = useJIT;
  this->useTraceJIT = useTraceJIT;
  emulatedTime = 0.0;
  binaryTime = 0.0;
  this->entryAddr = entryAddr;
  termAddr = 0;
  cnt = 0;
  hasBeenSplit = false;
  branchLikely = false;
  readOnly = false;
  this->bbMap = bbMap;
  this->insMap = insMap;
  (*bbMap)[entryAddr] = this;
}

bool basicBlock::hasSelfLoop()
{
  for(size_t i = 0; i < vecIns.size(); i++)
    {
      uint32_t bTarget;
      uint32_t inst = vecIns[i].first;
      uint32_t pc = vecIns[i].second;
      if(getStaticBranchTarget(inst, pc, bTarget))
	{
	  return (bTarget == entryAddr);
	}
    }
  return false;
}

void basicBlock::addIns(uint32_t inst, uint32_t addr)
{
  if(!readOnly)
    {
      /* 6/14/2014 TODO : This could lead to factorial 
       * blow up of memory in the worse case */

      /*
      if(insMap->find(addr) != insMap->end()) {
	printf("instruction at %x already exists in another basicblock\n", addr);
      }
      */
      std::pair<uint32_t, uint32_t> p(inst, addr);
      vecIns.push_back(p);
      (*insMap)[addr] = this;
    }
}


void basicBlock::setTermAddr(uint32_t termAddr)
{
  if(this->termAddr == 0)
    {
      this->termAddr = termAddr;
    }
}

basicBlock *basicBlock::split(uint32_t nEntryAddr)
{

  bbTraces.clear();
  for(std::set<basicBlock*>::iterator sit = cfgInTraces.begin();
      sit != cfgInTraces.end(); sit++) {
    basicBlock *nukeBB = *sit;
    //printf("==> nuking trace for %x\n", nukeBB->entryAddr);
    nukeBB->bbTraces.clear();
  }
  
  size_t offs = (nEntryAddr-entryAddr) / 4;
  
  bool splitAtLastInst = (offs == (vecIns.size()-1)) && 
    isBranchOrJump(vecIns[offs-1].first);

  /* Branching or jumping to the instruction in the
   * branch delay slot (who does this, seriously?) */

  if(splitAtLastInst)
    {
      //printf("Jump to branch delay slot : %x!!!\n", nEntryAddr);
      //print();
      basicBlock *nBB = new basicBlock(nEntryAddr,useJIT,useTraceJIT,insMap,bbMap);
      nBB->addIns(vecIns[offs].first, vecIns[offs].second);
      nBB->setReadOnly();
      nBB->termAddr = nEntryAddr;
      nBB->length = (nBB->termAddr-nBB->entryAddr)/4;
      return nBB;
    }

  hasBeenSplit = true;
  basicBlock *nBB = new basicBlock(nEntryAddr,useJIT,useTraceJIT,insMap,bbMap);


  //unlink old successors and update with new block
  for(std::set<basicBlock*>::iterator sit = succs.begin();
      sit != succs.end(); sit++)
    {
      basicBlock *b = *sit;
      std::set<basicBlock*>::iterator ssit = b->preds.find(this);
      //assert(ssit != b->preds.end());
      b->preds.erase(ssit);

      nBB->succsMap[b->entryAddr] = b;
      nBB->succs.insert(b);
      b->preds.insert(nBB);
    }
  //add new successor
  succs.clear();
  succsMap.clear();

  succsMap[nBB->entryAddr] = nBB;
  succs.insert(nBB);
  nBB->preds.insert(this);
  
  for(size_t i = offs; i < vecIns.size(); i++)
    {
      //update basicblock map
      uint32_t addr = vecIns[i].second;
      (*insMap)[addr] = nBB;
      nBB->vecIns.push_back(vecIns[i]);
    }
  
  vecIns.erase(vecIns.begin() + offs, vecIns.end());
  if(insFuncts.size() > offs)
    insFuncts.erase(insFuncts.begin() + offs, insFuncts.end());

  nBB->termAddr = termAddr;
  nBB->length = (nBB->termAddr-nBB->entryAddr)/4;

  termAddr = nEntryAddr-4;
  length = (termAddr-entryAddr)/4;
  readOnly = false;
  setReadOnly();

  //printf("AFTER:\n");
  //print();
  hasTermBranchOrJump = false;
  if( branchLikely ) 
    {
      nBB->branchLikely = true;
      branchLikely = false;
    }
  nBB->cnt = cnt;
  nBB->hasBeenSplit = true;
  nBB->setReadOnly(); 
  return nBB;
}

basicBlock *basicBlock::findBlock(uint32_t entryAddr)
{
#ifdef __USE_TIMING__
  uint64_t s0 = rdtsc();
#endif
  basicBlock *fBlock = 0;

  google::dense_hash_map<uint32_t, basicBlock*>::iterator sIt = 
    succsMap.find(entryAddr);

  if(sIt != succsMap.end())
    {
      fBlock = sIt->second;
    }
  else
    {
      std::map<uint32_t, basicBlock*>::iterator bIt = 
	bbMap->find(entryAddr);
      
      if(bIt != bbMap->end())
	{
	  fBlock = bIt->second;
	  /* replaced updateLinks()*/
	  succs.insert(fBlock);
	  succsMap[fBlock->entryAddr] = fBlock;
	  fBlock->preds.insert(this);
	}
      else
	{
	  std::map<uint32_t, basicBlock*>::iterator iIt = 
	    insMap->find(entryAddr);
	  
	  if(iIt != insMap->end())
	    {
	      basicBlock *sBB = iIt->second;
	      basicBlock *nBB = sBB->split(entryAddr);
	      fBlock = nBB;
	      /* replaced updateLinks() */
	      succs.insert(fBlock);
	      succsMap[fBlock->entryAddr] = fBlock;
	      fBlock->preds.insert(this);
	    }
	}
    }

#ifdef __USE_TIMING__
  s0 = rdtsc() - s0;
  findBlockCycles += s0;
#endif

#ifdef USE_PHT
  if(fBlock)
    fBlock->bPred = 0xffffffff;
#endif

  return fBlock;
}

void basicBlock::print()
{
  printf("block (%p) @ %x (cnt = %zu), split=%s, readOnly = %s, compiled=%s, selfLoop=%s\n", 
	 this, entryAddr, (size_t)cnt,
	 hasBeenSplit ? "yes" : "no",
	 readOnly ? "yes" : "no", 
	 isCompiled ? "yes" : "no",
	 hasSelfLoop() ? "yes" : "no");
  printf("%g sec in emulation, %g sec in binary translation\n",
	 emulatedTime, binaryTime);
  for(size_t i = 0; i < vecIns.size(); i++)
    {
      uint32_t inst = vecIns[i].first;
      uint32_t addr = vecIns[i].second;
      std::string asmString = getAsmString(inst, addr);
      printf("%x: %s\n", addr, asmString.c_str());
      if(addr == 0)
	{
	  printf("instruction has address 0, how?\n");
	  exit(-1);
	}
    }
  printf("terminal addr = %x\n", termAddr);
}

void basicBlock::fprint(FILE *fp)
{
  fprintf(fp, "block (%p) @ %x (cnt = %zu), split=%s, readOnly = %s, compiled=%s, selfLoop=%s\n", 
	 this, entryAddr, (size_t)cnt,
	 hasBeenSplit ? "yes" : "no",
	 readOnly ? "yes" : "no", 
	 isCompiled ? "yes" : "no",
	 hasSelfLoop() ? "yes" : "no");
  for(size_t i = 0; i < vecIns.size(); i++)
    {
      uint32_t inst = vecIns[i].first;
      uint32_t addr = vecIns[i].second;
      std::string asmString = getAsmString(inst, addr);
      fprintf(fp,"%x: %s\n", addr, asmString.c_str());
    }
}



void basicBlock::run(state_t *s)
{
  for(size_t i = 0; i <= length; i++)
    { 
      uint32_t inst = vecIns[i].first;
      exFunct foo = insFuncts[i];
      foo(inst, s);
    }
  s->icnt+=(length+1);
}

bool basicBlock::execute(state_t *s)
{
  if(!readOnly || s->pc != entryAddr)
    {
      s->oldpc = ~0;
      return false;
    }
  cnt++;
  s->oldpc = s->pc;

  this->run(s);

  if(s->pc != s->oldpc)
    {
      basicBlock *nBB = cBB->findBlock(s->pc);
      if(cBB == nBB)
	{
	  printf("cBB == nBB\n");
	  exit(-1);
	}
      if(nBB == 0) {
	nBB = new basicBlock(s->pc, cBB);
      }
      cBB = nBB;
    }
  return true;
}


basicBlock::~basicBlock()
{

}

void basicBlock::makeDot(std::string &fname)
{
  FILE *fp = fopen(fname.c_str(), "w");
  std::string s = "digraph g {\n";
  s += "graph [\n";
  s += "rankdir = \"LR\"\n";
  s += "];\n";
  s += "node [\n";
  s += "fontsize = \"16\"\n";
  s += "shape = \"ellipse\"\n";
  s += "];\n";
  s += "edge [\n";
  s += "];\n";

  /* add nodes */
  for(std::map<uint32_t, basicBlock*>::iterator mit = bbMap->begin();
      mit != bbMap->end(); ++mit)
    {
      basicBlock *bb = mit->second;
      s += "\"" + toString(bb->entryAddr) + "\" [\n";
      s += "label = \"";
      for(size_t i = 0; i < bb->vecIns.size(); i++)
	{
	  uint32_t inst = bb->vecIns[i].first;
	  uint32_t addr = bb->vecIns[i].second;
	  std::string asmString = getAsmString(inst, addr);
	  s += "<f" + toString(i) + ">" + " " + asmString;
	  if(i != (bb->vecIns.size()-1))
	    s += " | ";
	}
      s += "\"\n";
      s += "shape = \"record\"\n";
      s += "];\n";
      //delete mit->second;
    }
  /* add edges */
  for(std::map<uint32_t, basicBlock*>::iterator mit = bbMap->begin();
      mit != bbMap->end(); ++mit)
    {
      basicBlock *bb = mit->second;
      
      for(std::set<basicBlock*>::iterator sit = bb->succs.begin();
	  sit != bb->succs.end(); ++sit)
	{
	  basicBlock *nbb = *sit;
	  s += "\"" + toString(bb->entryAddr) + "\":f" + toString(bb->vecIns.size()-1) 
	    + " -> \"" + toString(nbb->entryAddr) + "\":f0";
	  s += "[ label=" + toString(1) +"];\n";
	}
    }

  s += "}\n"; 
  fprintf(fp,"%s", s.c_str());
  fclose(fp);
}

