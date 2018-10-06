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
#include <boost/program_options.hpp>

#include "loadelf.hh"
#include "helper.hh"
#include "parseMips.hh"
#include "profileMips.hh"
#include "globals.hh"

char **sysArgv = nullptr;
int sysArgc = 0;
bool enClockFuncts = false;


static state_t *s =0;

static int buildArgcArgv(const char *filename, const std::string &sysArgs, char **&argv){
  int cnt = 0;
  std::vector<std::string> args;
  char **largs = 0;
  args.push_back(std::string(filename));

  char *ptr = nullptr;
  char *c_str = strdup(sysArgs.c_str());
  if(sysArgs.size() != 0)
    ptr = strtok(c_str, " ");

  while(ptr && (cnt<MARGS)) {
    args.push_back(std::string(ptr));
    ptr = strtok(nullptr, " ");
    cnt++;
  }
  largs = new char*[args.size()];
  for(size_t i = 0; i < args.size(); i++) {
    const std::string & s = args[i];
    size_t l = strlen(s.c_str());
    largs[i] = new char[l+1];
    memset(largs[i],0,sizeof(char)*(l+1));
    memcpy(largs[i],s.c_str(),sizeof(char)*l);
  }
  argv = largs;
  free(c_str);
  return (int)args.size();
}

int main(int argc, char *argv[]) {
  bool bigEndianMips = true;
  namespace po = boost::program_options; 
#ifdef MIPSEL
  bigEndianMips = false;
#endif  
  fprintf(stderr, "%s%s INTERP: built %s %s%s\n",
	  KGRN, bigEndianMips ? "MIPS" : "MIPSEL" ,
	  __DATE__, __TIME__, KNRM);


  size_t pgSize = getpagesize();
  std::string sysArgs, filename;
  uint64_t maxinsns = ~(0UL);
  bool hash = false;
  try {
    po::options_description desc("Options");
    desc.add_options() 
      ("help", "Print help messages") 
      ("args,a", po::value<std::string>(&sysArgs), "arguments to mips binary") 
      ("clock,c", po::value<bool>(&enClockFuncts), "enable wall-clock")
      ("hash,h", po::value<bool>(&hash), "hash memory at end of execution")
      ("file,f", po::value<std::string>(&filename), "mips binary")
      ("maxinsns,m", po::value<uint64_t>(&maxinsns), "max instructions to execute")
      ; 
    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm); 
  }
  catch(po::error &e) {
    std::cerr << KRED << "command-line error : " << e.what() << KNRM << "\n";
    return -1;
  }

  if(filename.size()==0) {
    fprintf(stderr, "INTERP : no file\n");
    exit(-1);
  }

  /* Build argc and argv */
  sysArgc = buildArgcArgv(filename.c_str(),sysArgs,sysArgv);
  initParseTables();

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
  if(s->mem == nullptr) {
    std::cerr << "INTERP : couldn't allocate backing memory!\n";
    exit(-1);
  }
  
  load_elf(filename.c_str(), s);
  mkMonitorVectors(s);

  double runtime = timestamp();
  while(s->brk==0 and (s->icnt < s->maxicnt)) {
    execMips(s);
  }
  runtime = timestamp()-runtime;
  //std::cerr << *s << "\n";
  
  if(hash) {
    std::cerr << *s << "\n";
    std::cerr << "crc32=" << std::hex
	      << crc32(s->mem, 1UL<<32)<<std::dec
	      << "\n";
  }  


  fprintf(stderr, "%sINTERP: %g sec, %zu ins executed, %g megains / sec%s\n", 
	  KGRN, runtime, (size_t)s->icnt, s->icnt / (runtime*1e6), KNRM);
  
  munmap(mempt, 1UL<<32);
  if(sysArgv) {
    for(int i = 0; i < sysArgc; i++) {
      delete [] sysArgv[i];
    }
    delete [] sysArgv;
  }
  free(s);
  return 0;
}


