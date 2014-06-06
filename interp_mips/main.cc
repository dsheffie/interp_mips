#include <stdio.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdint.h>
#include <cstdlib>
#include <unistd.h>
#include <string>
#include <vector>
#include <cstring>
#include <cassert>

#include "loadelf.h"
#include "helper.h"
#include "parseMips.h"
#include "emulateMips.h"
std::string llog;


int main(int argc, char *argv[])
{
  int c,d=0,h=0,n=-1;
  bool enClockFuncts = false;
  uint8_t *mem = 0;
  uint32_t entry_p = 0, last_a;
  std::list<std::pair<uint32_t, uint32_t> > segs;
  double estart=0.0,estop=0.0;

   char *filename = NULL;
  while((c=getopt(argc,argv,"cdf:hn:"))!=-1)
    {
      switch(c)
	{
	case 'c':
	  enClockFuncts = true;
	  break;
	case 'd':
	  d = 1;
	  break;
	case 'f':
	  filename = optarg;
	  break;
	case 'h':
	  h = 1;
	  break;
	case 'n':
	  n = atoi(optarg);
	  break;
	}
    }

  if(filename==NULL)
    {
      printf("no file\n");
      exit(-1);
    }
  initParseTables();
  initEmulationTables(enClockFuncts);
  state_t *s = new state_t;
  initState(s);
  mem = new uint8_t[1UL<<32];
 
  load_elf(filename,
	   &entry_p,
	   &last_a,
	   segs,
	   mem);
  
  s->pc = entry_p;
  s->mem = mem;
  mkMonitorVectors(s);
  
  estart = timestamp();
  if(n==-1)
    {
      while(s->brk==0)
	{
	  execMips(s);
	}
    }
  else
    {
      do
	{
	  execMips(s);
	}
      while((s->icnt < n) && (s->brk==0));
    }
  estop = timestamp();
  //printState(s);

  FILE *fpl = fopen("log.txt", "w");
  fprintf(fpl, "%s", llog.c_str());
  fclose(fpl);
  
    
  double runtime = (estop-estart);
  printf("%g inst/sec (%g sec)\n", 
	 s->icnt / runtime,
	 runtime);

  if(h)
    {
      std::vector<uint8_t> hash;
      md5sum(mem, 1LU<<32, hash);
      printf("Hash:");
      for (size_t i = 0; i < hash.size(); i++) 
	{
	  printf("%.2x", hash[i]);
	}
      printf("\n");
    }

  delete [] mem;
  delete s;
  return 0;
}
