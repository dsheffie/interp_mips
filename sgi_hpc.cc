#include "sgi_hpc.hh"
#include "interpret.hh"

uint32_t sgi_hpc::read(uint32_t offs, size_t sz) {
  printf("%x unimplemented\n", offs);
  exit(-1);
  return 0;
}
