#include "sgi_hpc.hh"
#include "interpret.hh"
#include <cassert>

uint32_t sgi_hpc::read(uint32_t offs, size_t sz) {
  switch(offs)
    {
    case 0x30000:
      return intstat;
    case 0x30004:
      return misc;
    }

  printf("%s at pc %x : %x unimplemented\n", __PRETTY_FUNCTION__, s->pc, offs);
  exit(-1);
  return 0;
}

void sgi_hpc::write(uint32_t offs, uint32_t x, size_t sz) {
  switch(offs) {
  case 0x30004:
    misc = x&3;
    break;
  default:
    assert(false);
  }
}
