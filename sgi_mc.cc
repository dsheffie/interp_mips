#include "sgi_mc.hh"
#include "helper.hh"
#include "interpret.hh"
#include <cstdio>

/* https://erikarn.github.io/sgi/indy/datasheets/sgi_indy_mc.pdf */

/*
All of the MC registers will respond to two different addresses. It is up
to the programmer to use the correct address depending on the endian mode of
the processor.

The MC is connected to the least significant 32 bits of the sysad bus.
When a register is written the data must be driven on those bits.
When register is read the data will be returned on those pins as well.

If the processor is running in big endian mode the odd word addresses,
(addresses that end in 4 and 0xc) are used.

When the processor is running in little endian mode the even word addresses,
(addresses that end in 0 and 8) are used.
*/
  
uint32_t sgi_mc::read(uint32_t offs, size_t sz) {
  uint32_t x = 0;
  switch(offs)
    {
    case 0x0:
    case 0x4:
    case 0x8:
    case 0xc: {
      const uint32_t index = (offs >> 1) & 1;
      x = cpu_control[index];
      break;
    }
    case 0xc4: 
    case 0xcc: {
      const uint32_t index = (offs >> 1) & 1;
      x = memcfg[index];
      break;
    }      
    case 0xd4:
      x = cpu_mem_access_config;
      break;
    case 0xdc:
      x = gio_mem_access_config;
      break;
    case 0x30:
      x = 0x10;//eeprom_ctrl & (~0x10);
      break;
    case 0x1004:
      //printf("rpss counter read\n");
      x = static_cast<uint32_t>(s->icnt/10);//rpss_counter;
      break;
    default:
      printf("trying to read reg %x\n", offs);
      exit(-1);
      break;
    }
  //printf("read access to MC, reg %x, value %x\n", offs, x);  
  return x;
}

uint32_t nbits = 0;
static uint8_t eerom[256] = {0};
static uint8_t byte = 0;
static uint32_t cbyte = 0;

void sgi_mc::write(uint32_t offs, uint32_t x, size_t sz) {
  //printf("write access to MC, reg %x, value %x, size %lu\n", offs, x, sz);
  
  switch(offs)
    {
    case 0x0:
    case 0x4:
    case 0xc: {
      const uint32_t index = (offs >> 1) & 1;
      cpu_control[index] = x;
      break;
    }
    case 0x2c:
      rpss_divider = x;
      break;
    case 0x84:
      gio64_arb_param = x;
      break;
    case 0xc4: 
    case 0xcc: {
      const uint32_t index = (offs >> 1) & 1;
      memcfg[index] = x;
      break;
    }      
    case 0xd4:
      cpu_mem_access_config = x;
      break;
    case 0xdc:
      gio_mem_access_config =x;
      break;
    case 0xec:
      cpu_error_status = 0;
      break;
    case 0xfc:
      gio_error_status = 0;
      break;
    case 0x30:
      eeprom_ctrl = x;
      if ( ((x>>1) & 3) == 3) {
	printf("data bit %d, bit %u\n", (x>>3)&1, nbits);
	byte = (byte << 1) | ((x>>3)&1);
	++nbits;	
	if(nbits==8) {
	  printf("wrote byte %u : %x\n", cbyte, (int)byte);	  
	  eerom[cbyte] = byte;
	  ++cbyte;
	  nbits = 0;
	  byte = 0;

	}

      }
      break;
    default:
      exit(-1);
      break;
    }
}
