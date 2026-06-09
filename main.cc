#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <iostream>
#include <string>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <boost/program_options.hpp>

#include "interpret.hh"
#include "loadelf.hh"
#include "sparse_mem.hh"
#include "sgi_mc.hh"
#include "sgi_hpc.hh"
#include "globals.hh"

namespace po = boost::program_options;
namespace globals {
  bool enClockFuncts = false;
  uint64_t icountMIPS = 0;
  uint64_t cycle = 0;
  bool trace_retirement = false;
  bool trace_fp = false;
  bool report_syscalls = false;
};
static state_t *s = nullptr;

int main(int argc, char *argv[]) {
  std::string filename, arcs;
  uint64_t maxinsns = ~(0UL);
  try {
    po::options_description desc("options");
    desc.add_options()
      ("file,f",    po::value<std::string>(&filename), "ELF kernel image")
      ("arcs",      po::value<std::string>(&arcs),     "ARCS firmware blob (loaded at PA 0x1000)")
      ("maxicnt,m", po::value<uint64_t>(&maxinsns)->default_value(~(0UL)), "max instructions");
    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);
  } catch(...) { std::cerr << "bad args\n"; return 1; }

  if(filename.empty()) { std::cerr << "need --file <kernel>\n"; return 1; }

  sparse_mem *sm = new sparse_mem();
  s = new state_t(*sm);
  s->maxicnt = maxinsns;
  s->mc  = new sgi_mc(s);
  s->hpc = new sgi_hpc(s);
  sm->st = s;
  sm->route_devices = true;

  initState(s);                    /* CP0 reset state: PRId, Config, SR, Random */
  load_elf(filename.c_str(), s);   /* sets s->pc to the entry */

  if(!arcs.empty()) {
    int fd = open(arcs.c_str(), O_RDONLY);
    if(fd < 0) { std::cerr << "cannot open arcs " << arcs << "\n"; return 1; }
    struct stat st; fstat(fd, &st);
    void *buf = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    memcpy(sm->mem + 0x1000, buf, st.st_size);
    std::cerr << "loaded ARCS firmware (" << st.st_size << " bytes) at PA 0x1000\n";
    munmap(buf, st.st_size); close(fd);
  }

  while(s->brk == 0 && s->icnt < s->maxicnt) {
    execMips(s);
  }
  std::cout << "\n" << s->icnt << " instructions executed, brk=" << (int)s->brk << "\n";

  delete s->mc; delete s->hpc; delete s; delete sm;
  return 0;
}
