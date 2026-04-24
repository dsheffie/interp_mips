#include "sgi_hpc.hh"
#include "interpret.hh"
#include <cassert>

uint32_t sgi_hpc::read(uint32_t offs, size_t sz) {
  if(offs <= 0x0000ffff) {
    printf("pbus dma read\n");
  }
  else if(offs >= 0x00010000 and offs <= 0x0001ffff) {
    printf("enet read\n");
  }
  else if(offs == 0x30000) {
    return intstat;
  }
  else if(offs == 0x30004) {
    return misc;
  }
  else if(offs >= 0x00058000 and offs <= 0x0005bfff) {
    uint32_t c = (offs >> 8) & 15;
    if (c >= 10) {
      c = (c & 1) ? 9 : 8;
    }
    printf("pio data on channel %u\n", c);
  }
  //else {
  printf("%s at pc %x : %x unimplemented\n", __PRETTY_FUNCTION__, s->pc, offs);    
  assert(false);
    //}
  
  //exit(-1);
  return 0;
}

void sgi_hpc::write(uint32_t offs, uint32_t x, size_t sz) {
  if(offs <= 0x0000ffff) {
    printf("pbus dma write\n");
  }
  else if(offs >= 0x00010000 and offs <= 0x0001ffff) {
    printf("enet write\n");
  }
  else if(offs == 0x30004) {
    misc = x&3;
  }
  else if(offs >= 0x00058000 and offs <= 0x0005bfff) {
    uint32_t c = (offs >> 8) & 15;
    if (c >= 10) {
      c = (c & 1) ? 9 : 8;
    }
    printf("pio data on channel %u\n", c);    
  }
  else {
    printf("%s at pc %x : %x unimplemented\n", __PRETTY_FUNCTION__, s->pc, offs);    
    assert(false);
  }
}
