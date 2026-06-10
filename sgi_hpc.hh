#ifndef __sgi_hpc__
#define __sgi_hpc__

#include <cstdint>
#include <cstddef>

struct state_t;

/* SGI Indy HPC3 peripheral controller (first chip, base PA 0x1fb80000).
 * Backing store for the registers; full map and bit defs in sgi_hpc.cc. */
class sgi_hpc {
  state_t *s;
  uint32_t intstat;                  /* reg 0x30000 (istat0): interrupt status [4:0] */
  uint32_t misc;                     /* reg 0x30004 (gio_misc) */
  uint32_t pbus_pio_config[10] = {0};/* reg 0x5d000 block: per-channel PIO config (10 ch) */
  uint32_t pbus_dma_config[8] = {0}; /* reg 0x5c000 block: per-channel DMA config (8 ch) */
public:
  sgi_hpc(state_t *s) : s(s), intstat(0), misc(0) {}
  uint32_t read(uint32_t offs, size_t sz);
  void write(uint32_t offs, uint32_t x, size_t sz);  
};


#endif
