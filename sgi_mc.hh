#ifndef __sgi_mc__
#define __sgi_mc__

#include <cstdint>
#include <cstddef>

/* systemid value (MC reg 0x1c): [3:0]=revision, [4]=EISA present. Indy has no
 * EISA so bit4 stays clear; MAME reports revision 3 ("rev c"). */
static const uint32_t sys_id = 0x3; /* mame says rev c */

struct state_t;

/* SGI Indy Memory Controller. Backing store for the MC register window; see
 * the register map and bit definitions in sgi_mc.cc. */
class sgi_mc {
  state_t *s;
  uint32_t eeprom_ctrl;           /* reg 0x30/0x34: serial EEPROM bit-bang state */
  uint32_t cpu_error_status;      /* reg 0xec (cstat) */
  uint32_t gio_error_status;      /* reg 0xfc (gstat) */
  uint32_t cpu_mem_access_config; /* reg 0xd4 (cmacc) */
  uint32_t gio_mem_access_config; /* reg 0xdc (gmacc) */
  uint32_t gio64_arb_param;       /* reg 0x84 (giopar) */
  uint32_t rpss_divider;          /* reg 0x2c (divider) */
  uint32_t rpss_counter;          /* reg 0x1004 (rpss free-running counter) */
  uint32_t cpu_control[2] = {0};  /* reg 0x04/0x0c (cpuctrl0/cpuctrl1) */
  uint32_t memcfg[2] = {0};       /* reg 0xc4/0xcc (mconfig0/mconfig1) */
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


