#ifndef __sgi_mc__
#define __sgi_mc__

#include <cstdint>
#include <cstddef>

static const uint32_t sys_id = 0x3; /* mame says rev c */  

struct state_t;

class sgi_mc {
  state_t *s;
  uint32_t eeprom_ctrl;
  uint32_t cpu_error_status;
  uint32_t gio_error_status;
  uint32_t cpu_mem_access_config;
  uint32_t gio_mem_access_config;
  uint32_t gio64_arb_param;
  uint32_t rpss_divider;
  uint32_t rpss_counter;
  uint32_t cpu_control[2] = {0};
  uint32_t memcfg[2] = {0};
public:
  sgi_mc(state_t *s) : s(s),
		       eeprom_ctrl(0),
		       cpu_error_status(0),
		       gio_error_status(0),
		       cpu_mem_access_config(0),
		       gio_mem_access_config(0),
		       gio64_arb_param(0),
		       rpss_divider(0x104),
		       rpss_counter(0) {

  };
  uint32_t read(uint32_t offs, size_t sz);
  void write(uint32_t offs, uint32_t x, size_t sz);  
};


#endif


