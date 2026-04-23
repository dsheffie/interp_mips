#include <cstdio>
#include <iostream>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <signal.h>
#include <cxxabi.h>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <cstring>
#include <cassert>
#include <map>
#include <fstream>
#include <boost/program_options.hpp>

#include "loadelf.hh"
#include "helper.hh"
#include "disassemble.hh"
#include "interpret.hh"
#include "saveState.hh"
#include "globals.hh"

extern const char* githash;

bool globals::enClockFuncts = false;
bool globals::isMipsEL = false;
bool globals::log = false;
uint64_t globals::icountMIPS = 500;
bool globals::silent = true;
std::map<uint32_t, uint64_t> globals::execHisto;

static state_t *s =0;

template<typename X, typename Y>
static inline void dump_histo(const std::string &fname,
			      const std::map<X,Y> &histo) {
  if(histo.empty())
    return;
  
  std::vector<std::pair<X,Y>> sorted_by_cnt;
  for(auto &p : histo) {
    sorted_by_cnt.emplace_back(p.second, p.first);
  }
  std::ofstream out(fname);
  std::sort(sorted_by_cnt.begin(), sorted_by_cnt.end());
  for(auto it = sorted_by_cnt.rbegin(), E = sorted_by_cnt.rend(); it != E; ++it) {
    uint32_t pc = it->second;
    uint32_t r_inst = *reinterpret_cast<uint32_t*>(s->mem+pc);
    r_inst = bswap<false>(r_inst);	
    auto s = getAsmString(r_inst, it->second);
    out << std::hex << it->second << ":"
  	      << s << ","
  	      << std::dec << it->first << "\n";
  }
  out.close();
}


int main(int argc, char *argv[]) {
  bool bigEndianMips = true;
  namespace po = boost::program_options; 
  std::string dumpname;
  int64_t dumpIcnt = -1L;
  size_t pgSize = getpagesize();
  std::string sysArgs, filename;
  uint64_t maxinsns = ~(0UL);
  bool hash = false, isDump = false;

  try {
    po::options_description desc("Options");
    desc.add_options() 
      ("help", "Print help messages") 
      ("args,a", po::value<std::string>(&sysArgs),
       "arguments to mips binary") 
      ("clock,c", po::value<bool>(&globals::enClockFuncts)->default_value(false),
       "enable wall-clock")
      ("hash,h", po::value<bool>(&hash)->default_value(false),
       "hash memory at end of execution")
      ("file,f", po::value<std::string>(&filename), "mips binary")
      ("isdump,d", po::value<bool>(&isDump)->default_value(false), "is a dump")
      ("dumpicnt", po::value<int64_t>(&dumpIcnt)->default_value(-1L),
       "dump after n instructions")
      ("dumpname", po::value<std::string>(&dumpname), "dump file name")
      ("maxicnt,m", po::value<uint64_t>(&maxinsns)->default_value(~(0UL)),
       "max instructions to execute")
      ("silent,s", po::value<bool>(&globals::silent)->default_value(true),
       "no interpret messages")
      ("log", po::value<bool>(&globals::log)->default_value(false), "log instr")
      ; 
    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm); 
  }
  catch(po::error &e) {
    std::cerr << KRED << "command-line error : " << e.what() << KNRM << "\n";
    return -1;
  }

  
  if(not(globals::silent)) {
    std::cerr << KGRN
	      << "MIPS INTERP : built "
	      << __DATE__ << " " << __TIME__
	      << ",pid="<< getpid() << "\n"
	      << "git hash=" << githash
	      << KNRM << "\n";
  }
  

  /* Build argc and argv */

  int rc = posix_memalign((void**)&s, pgSize, pgSize); 
  initState(s);
  s->maxicnt = maxinsns;
#ifdef __linux__
  void* mempt = mmap(nullptr, 1UL<<32, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
#else
  void* mempt = mmap(nullptr, 1UL<<32, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS , -1, 0);
#endif
  assert(mempt != reinterpret_cast<void*>(-1));
  assert(madvise(mempt, 1UL<<32, MADV_DONTNEED)==0);
  s->mem = reinterpret_cast<uint8_t*>(mempt);
  s->mc = new sgi_mc(s->mem);
  
  if(s->mem == nullptr) {
    std::cerr << "INTERP : couldn't allocate backing memory!\n";
    exit(-1);
  }

  {
    struct stat ss;
    int fd = open("ip20prom.070-8116-004.BE.bin", O_RDONLY);
    if(fd<0) {
      printf("INTERP: open() returned %d\n", fd);
      exit(-1);
    }
    rc = fstat(fd,&ss);
    if(rc<0) {
      printf("INTERP: fstat() returned %d\n", rc);
      exit(-1);
    }
    char *buf = (char*)mmap(nullptr, ss.st_size,
			    PROT_READ, MAP_PRIVATE, fd, 0);

    memcpy(s->mem+(0xbfc00000 & 0x1fffffff), buf, ss.st_size);
    
    s->pc = 0xbfc00000;
    close(fd);
  }

  
  
  initCapstone();

  double runtime = timestamp();
  
  if(globals::isMipsEL) {
    while(s->brk==0 and (s->icnt < s->maxicnt)) {
      execMipsEL(s);
    }
  }
  else {
    if(dumpIcnt != -1L) {
      if(dumpname.size() == 0) {
	dumpname = filename;
      }
      while(s->brk==0 and (s->icnt < s->maxicnt)) {
	if(((s->icnt % dumpIcnt) == 0) and (s->icnt != 0)) {
	  std::stringstream ss;
	  ss << dumpname << s->icnt << ".bin";
	  if(not(globals::silent)) {
	    std::cout << "dumping at icnt " << s->icnt << "\n";
	  }
	  dumpState(*s, ss.str());
	  s->brk = 1;
	}
	execMips(s);
      }
    }
    else {
      while(s->brk==0 and (s->icnt < s->maxicnt)) {
	execMips(s);
      }
    }
  }
  runtime = timestamp()-runtime;
  dump_histo("exec.txt", globals::execHisto);
  
  if(hash) {
    std::fflush(nullptr);
    /* std::cerr << *s << "\n"; */
    std::cerr << "crc32=" << std::hex
	      << crc32(s->mem, 1UL<<32)<<std::dec
	      << "\n";
  }  


  if(not(globals::silent)) {
    std::cerr << KGRN << "INTERP: "
	      << runtime << " sec, "
	      << s->icnt << " ins executed, "
	      << s->nopcnt << " nops executed, "
	      << std::round((s->icnt/runtime)*1e-6) << " megains / sec "
	      << KNRM  << "\n";
  }

  
  munmap(mempt, 1UL<<32);
  delete s->mc;
  free(s);
  stopCapstone();

  return 0;
}


