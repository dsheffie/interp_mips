#ifndef __sgi_mc__
#define __sgi_mc__

#include <cstdint>
#include <cstddef>
#include <cstdlib>

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
    /* Advertise installed DRAM as the IP22 PROM would, so the kernel MC probe
     * finds it.  bank0 = mconfig0[31:16]; MC rev<5 decode: base=(cfg&0xff)<<22,
     * size=((cfg&0x1f00)+0x100)<<14.  cfg = BVALID(0x2000)|RMASK|BASE(0x20).
     * The kernel reads mconfig0/1 directly in mlreset() at the +4/+c BE aliases
     * (0x1fa000c4 / 0x1fa000cc); stored byte-swapped so the kernel reads
     * 0x23200000 / 0.
     *
     * Default = 16 MiB @ PA 0x08000000 (cfg 0x2320, size field [12:8]=0x03) --
     * this is the real SGI Indy / MAME config (confirmed via MAME hinv; see
     * MAME_QUESTIONS.md Q4). NB: 128 MiB (cfg 0x3f20, field 0x1f) makes IRIX's
     * VM init skip init_pmap and panic with no banner; 16 MiB lets init_pmap run
     * and IRIX print its release banner.
     * MEMCFG=<hex cfg> overrides at runtime. */
    const char *e = getenv("MEMCFG");
    uint32_t cfg = e ? (uint32_t)strtoul(e, nullptr, 16) : 0x2320u;
    memcfg[0] = __builtin_bswap32(cfg << 16);
    memcfg[1] = 0;
  };
  uint32_t read(uint32_t offs, size_t sz);
  void write(uint32_t offs, uint32_t x, size_t sz);  
};


#endif


