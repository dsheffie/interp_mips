#include "sgi_mc.hh"
#include "helper.hh"
#include <cstdio>

/* https://erikarn.github.io/sgi/indy/datasheets/sgi_indy_mc.pdf */

uint32_t sgi_mc::read(uint32_t offs) {
  uint32_t x = 0;
  switch(offs)
    {
    case 0x30:
      x = eeprom_ctrl & (~0x10);
      break;
    default:
      exit(-1);
      break;
    }
  printf("read access to MC, reg %x, value %x\n", offs, x);  
  return x;
}

uint32_t nbits = 0;
void sgi_mc::write(uint32_t offs, uint32_t x) {
  printf("write access to MC, reg %x, value %x\n", offs, x);
  switch(offs)
    {
    case 0x30:
      eeprom_ctrl = x;
      if ( ((x>>1) & 3) == 3) {
	printf("data bit %d, bit %u\n", (x>>3)&1, nbits);
	++nbits;
      }
      break;
    default:
      exit(-1);
      break;
    }
}
