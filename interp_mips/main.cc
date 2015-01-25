#include <stdio.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdint.h>
#include <cstdlib>
#include <unistd.h>
#include <string>
#include <cstring>
#include <cassert>
#include <sys/mman.h>
#include <signal.h>
#include <cxxabi.h>
#include <sigsegv.h>

#include "loadelf.hh"
#include "helper.hh"
#include "parseMips.hh"
#include "profileMips.hh"
#include "basicBlock.hh"
#include "simCache.hh"


#include "globals.hh"


/* Globals */
std::vector<uint64_t> instAbortCounts;
bool enComplDouble = true;
bool enComplSingle = true;

simCache *dCache = 0;
size_t bbHotThresh = 50;
bool server = false;
uint64_t traceCycles = 0;
uint64_t jitCycles = 0;
uint64_t interpCycles = 0;
uint64_t traceCompileCycles = 0;
uint64_t blockCompileCycles = 0;
size_t totalCycles = 0;
size_t execCycles = 0;
size_t findBlockCycles = 0;
size_t updateLinksCycles = 0;
size_t numInstBlock = 0;
size_t numInstTrace = 0;
std::map<uint32_t, uint8_t*> dirtyPageMap;
int sArgc = -1;
char** sArgv = NULL;

std::string *executeLog;


static state_t *s =0;
int buildArgcArgv(char *filename, char *sysArgs, char ***argv);
void catchSIGSEGV(int sig)
{
  printf("jumped to SEGSEGV handler\n");
  if(s)
    {
      /*printf("we had jumped to a compiled block @ %x\n", s->oldpc);*/
      uint32_t inst = accessBigEndian(*(uint32_t*)(s->mem + s->pc));
      std::string badInstr = getAsmString(inst, s->pc);
      printf("segfault due to %s\n", badInstr.c_str());
      printState(s);
    }

  exit(-1);
}

int handle_pgfault(void *fault_addr, int serious)
{
  uint64_t pg_mask = ~(4095UL);
  uint64_t uaddr = (uint64_t)fault_addr;
  uint8_t *pg_addr = (uint8_t*)(uaddr & pg_mask);
  int64_t pg_offs = ((int64_t)pg_addr - (int64_t)s->mem) >> 12;
  

  if(s->pgstate[pg_offs] == PROT_NONE) {
    s->pgstate[pg_offs] = PROT_READ;
    mprotect((void*)pg_addr, 4096, PROT_READ);
    return 1;
  }
  else if(s->pgstate[pg_offs] == PROT_READ) {
    if(s->mode) {
      uint8_t *cpy = new uint8_t[4096];
      memcpy(cpy, &(s->mem[4096*pg_offs]), 4096);
      dirtyPageMap[(uint32_t)pg_offs] = cpy;
    }
    s->pgstate[pg_offs] = PROT_READ | PROT_WRITE;
    mprotect((void*)pg_addr, 4096, PROT_READ | PROT_WRITE);
    return 1;
  }
  else {
    printf("i don't know how do handle this..\n");
    abort();
    return 0;
  }

  //if(mprotect((void*)pg_addr, 4096, PROT_READ | PROT_WRITE) == 0) {
  //s->pgstate[pg_offs] = (PROT_READ | PROT_WRITE);
  // return 1;
  //} 
  //else {
  // abort();
  // return 0;
  //}

}


void trapEqAbort()
{
  printf("teq abort from %x\n", s->pc);
  exit(-1);
}



static void report()
{
#ifdef __USE_TIMING__
  size_t overheadCycles =totalCycles - (traceCycles+jitCycles+interpCycles+traceCompileCycles+blockCompileCycles);
  fprintf(stderr,"JIT: total host cycles        = %zu\n", totalCycles);
  fprintf(stderr,"JIT: host cycles in trace     = %zu\n", (size_t)traceCycles);
  fprintf(stderr,"JIT: host cycles in block     = %zu\n", (size_t)jitCycles);
  fprintf(stderr,"JIT: host cycles in interp    = %zu\n", (size_t)interpCycles);
  fprintf(stderr,"JIT: host cycles in block cpl = %zu\n", (size_t)blockCompileCycles);
  fprintf(stderr,"JIT: host cycles in trace cpl = %zu\n", (size_t)traceCompileCycles);
  fprintf(stderr,"JIT: overhead cycles          = %zu\n", overheadCycles);
  fprintf(stderr,"JIT: cycles in execute()      = %zu\n", execCycles);
  fprintf(stderr,"JIT: cycles in findBlock()    = %zu\n", findBlockCycles);
  fprintf(stderr,"JIT: cycles in updateLinks()  = %zu\n", updateLinksCycles);
  fprintf(stderr,"\n\n");
  size_t numInstInterp = s->icnt - (numInstBlock+numInstTrace);

  fprintf(stderr,"JIT: total dynamic ins cnt    = %zu\n", (size_t)s->icnt);
  fprintf(stderr,"JIT: dynamic ins in block JIT = %zu\n", numInstBlock);
  fprintf(stderr,"JIT: dynamic ins in trace JIT = %zu\n", numInstTrace);
  fprintf(stderr,"JIT: dynamic ins interpreted  = %zu\n", numInstInterp);
  fprintf(stderr,"\n\n");

  if(jitCycles) {
    double bips = (double)numInstBlock / (double)jitCycles;
    fprintf(stderr,"JIT: block JIT   %g ins/cycle\n", bips);
  }
  if(traceCycles) {
    double tips = (double)numInstTrace / (double)traceCycles;
    fprintf(stderr,"JIT: trace JIT   %g ins/cycle\n", tips);
  }
  if(interpCycles) {
    double iips = (double)numInstInterp / (double)interpCycles;
    fprintf(stderr,"JIT: interpreter %g ins/cycle\n", iips);
  }
  double aips = (double)s->icnt / (double)totalCycles;
  fprintf(stderr,"JIT: avg         %g ins/cycle\n", aips);
#endif
}

void catchSIGINT(int sig)
{
  if(s) {
    //report();
    printState(s);
  }
  exit(-1);
}

int main(int argc, char *argv[])
{
  
  basicBlock *eBB = 0;
  bool useDCache = false;
  bool useJIT = false;
  bool useTraceJIT = false;
  bool enLog = false;
  int c,h=0,n=-1,silent=1;
  char **sysArgv = 0;
  int sysArgc = 0;
  int ipc = -1;
  size_t pgSize = getpagesize();
  lowAssocCache *l3D = 0;
  lowAssocCache *l2D = 0;
  lowAssocCache *l1D = 0;

  /* From HP Dynamo */
  size_t hotThresh = 50;
  bbHotThresh = (useJIT && useTraceJIT) ? 500 : bbHotThresh;

  bool enClockFuncts = false;
  uint8_t *mem = 0;
  uint32_t entry_p = 0, last_a;
  std::list<std::pair<uint32_t, uint32_t> > segs;

  std::map<uint32_t, basicBlock*> *bbMap = 
    new std::map<uint32_t, basicBlock*>;
  std::map<uint32_t, basicBlock*> *insMap = 
    new std::map<uint32_t, basicBlock*>;


  size_t numInstr = (inst_unknown - inst_monitor) + 1;
  instAbortCounts.resize(numInstr, 0);
  //printf("numInstr = %zu, instAbortCounts = %zu\n", 
  //numInstr, instAbortCounts.size());

  double estart=0.0,estop=0.0;

  char *filename = NULL;
  char *sysArgs = NULL;

  uint32_t l1d_lines = 1<<6;
  while((c=getopt(argc,argv,"a:cd:f:hi:jln:s:t"))!=-1)
    {
      switch(c)
	{
	case 'a':
	  sysArgs = strdup(optarg);
	  break;
	case 'c':
	  enClockFuncts = true;
	  break;
	case 'd':
	  useDCache = true;
	  l1d_lines = 1<<(uint32_t)atoi(optarg);
	  break;
	case 'f':
	  filename = optarg;
	  break;
	case 'h':
	  h = 1;
	  break;
	case 'j':
	  useJIT = true;
	  break;
	case 'l':
	  enLog = true;
	  break;
	case 'n':
	  n = atoi(optarg);
	  break;
	case 's':
	  silent = atoi(optarg);
	  break;
	case 't':
	  useTraceJIT = true;
	  break;
	}
    }

 
  if(filename==NULL)
    {
      fprintf(stderr, "JIT: no file\n");
      exit(-1);
    }
  if(enLog) {
    executeLog = new std::string();
  } else {
    executeLog = 0;
  }

  if(useDCache) {
    /*
    l3D = new lowAssocCache(64, 16, (1<<13), "l3D", 40, NULL);
    l2D = new lowAssocCache(64, 8, (1<<9), "l2D", 12, l3D); 
    l1D = new lowAssocCache(64, 8, (1<<6), "l1D", 3, l2D);     
    */
    l1D = new lowAssocCache(64, 8, l1d_lines, "l1D", 1, NULL);     
    dCache = l1D;
  }

  /* Build argc and argv */
  sysArgc = buildArgcArgv(filename,sysArgs,&sysArgv);
  
  initParseTables();
  initEmulationTables(enClockFuncts, sysArgc, sysArgv);

  int rc = posix_memalign((void**)&s, pgSize, pgSize); 
  initState(s);
  
  rc = posix_memalign((void**)&mem, pgSize, (1UL<<32));
  if(rc != 0) {
    fprintf(stderr, "JIT: couldn't allocate backing memory!\n");
    exit(-1);
  }
  rc = posix_memalign((void**)&s->pgstate, pgSize, sizeof(int)*(1<<20));
  if(rc != 0) {
    fprintf(stderr, "JIT: couldn't allocate page metadata!\n");
    exit(-1);
  }
  s->mem = mem;


  load_elf(filename,
	   &entry_p,
	   &last_a,
	   segs,
	   mem);

  /* make all MIPS memory no-read, no-write */
  //initPgState(s);
  //sigsegv_install_handler(&handle_pgfault);

  
  cBB = eBB = new basicBlock(entry_p, useJIT, useTraceJIT, 
			     insMap, bbMap);
  
  s->pc = entry_p;

  mkMonitorVectors(s);
  
  estart = timestamp();
  totalCycles = rdtsc();
  if(!(useJIT || useTraceJIT)) {
    while(s->brk==0) 
      {
	execMips(s);
      }
  }
  else
    {
      while(s->brk==0) 
	{
#ifdef __USE_TIMING__
	  uint64_t s0 = rdtsc();
#endif

	  bool ex = cBB->execute(s);	
#ifdef __USE_TIMING__
	  s0 = rdtsc() - s0;
	  execCycles += s0;
#endif
	  if(!ex)
	    {
#ifdef __USE_TIMING__
	      uint64_t s1 = rdtsc();
#endif
	      execMips(s);
#ifdef __USE_TIMING__
	      s1 = rdtsc() - s1;
	      interpCycles += s1;
#endif
	    }
	}
    }
  totalCycles = rdtsc() - totalCycles;
  estop = timestamp();
    
  double runtime = (estop-estart);


  fprintf(stderr, "JIT: %g sec, %zu ins executed : %g megains / sec\n", 
	  runtime,
	  (size_t)s->icnt, 
	  s->icnt / (runtime*1e6));

  if(!silent)
    {
      printState(s);
    }

  if(dCache) {
    dCache->getStats();
    fprintf(stderr,"JIT: AMAT = %g\n", dCache->computeAMAT());
  }


  for(std::map<uint32_t, basicBlock*>::iterator mit = bbMap->begin();
      mit != bbMap->end(); ++mit)
    {
      delete mit->second;
    }

  if(h)
    {
      uint32_t crc = crc32(mem, 1LU<<32);
      fprintf(stderr, "crc32=%x\n",crc);
    }

  report();
  
  //std::map<size_t, std::string> Insn::nameToIdHash;
  //std::map<size_t, uint64_t> Insn::compiledInstCounts;
  
  /*
  for(std::map<size_t, uint64_t>::iterator mit = Insn::compiledInstCounts.begin();
      mit != Insn::compiledInstCounts.end(); mit++) {
    std::string name = Insn::nameToIdHash[mit->first];
    int status = 0;
    char *realname = abi::__cxa_demangle(name.c_str(), 0, 0, &status);
    printf("%s : %zu\n", realname, (size_t)mit->second);
    free(realname);
  }
  */

  /*
  for(size_t i = 0; i < instAbortCounts.size(); i++)
    {
      size_t nAborts = (size_t)instAbortCounts[i];
      if(nAborts > 0)
	{
	  fprintf(stderr,"%s caused %zu aborts\n", 
		 getInstTypeStr(i).c_str(), nAborts);
	}
    }
  */

  free(mem);
  if(sysArgs) {
    free(sysArgs);
  }
  if(sysArgv) {
    for(int i = 0; i < sysArgc; i++) {
      delete [] sysArgv[i];
    }
    delete [] sysArgv;
  }

  if(enLog)
    {
      std::string logname = std::string(filename) + ".itrace";
      FILE *fp = fopen(logname.c_str(), "w");
      fprintf(fp, "%s", executeLog->c_str());
      delete executeLog;
    }
  /*
  size_t touchedpgs = 0;
  for(size_t i = 0; i < 1<<20; i++) {
    touchedpgs += !(s->pgstate[i] == PROT_NONE);
  }
  printf("touched %zu pages\n", touchedpgs);
  */

  free(s->pgstate);
  free(s);
  delete bbMap;
  delete insMap;


  if(l3D)
    delete l3D;
  if(l2D)
    delete l2D;
  if(l1D)
    delete l1D;
   
  return 0;
}

int buildArgcArgv(char *filename, char *sysArgs, char ***argv)
{
  int cnt = 0;
  std::vector<std::string> args;
  char **largs = 0;
  args.push_back(std::string(filename));

  char *ptr = strtok(sysArgs, " ");
  while(ptr && (cnt<MARGS))
    {
      args.push_back(std::string(ptr));
      ptr = strtok(NULL, " ");
      cnt++;
    }
  largs = new char*[args.size()];
  for(size_t i = 0; i < args.size(); i++)
    {
      std::string s = args[i];
      size_t l = strlen(s.c_str());
      largs[i] = new char[l+1];
      memset(largs[i],0,sizeof(char)*(l+1));
      memcpy(largs[i],s.c_str(),sizeof(char)*l);
    }
  *argv = largs;
  return (int)args.size();
}
