#ifndef __sgi_mc__
#define __sgi_mc__

#include <cstdint>

static const uint32_t sys_id = 0x3; /* mame says rev c */  

class sgi_mc {
  uint8_t *mem;
  uint32_t eeprom_ctrl;

public:
  sgi_mc(uint8_t *mem) : mem(mem), eeprom_ctrl(0) {

  };
  uint32_t read(uint32_t offs);
  void write(uint32_t offs, uint32_t x);  
};


#endif


