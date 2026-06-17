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
  /* i8254 PIT counter 2 (IP22 timer calibration; tcnt2=offs 0x598bb,
   * tcword=offs 0x598bf).  ip22-time.c:dosample() programs cnt2, then polls the
   * latched value until its high byte reads 0, measuring CP0 Count across that
   * interval to calibrate mips_hpt_frequency.  We tie the down-count to CP0
   * Count (which ticks once per retired insn) so the poll converges and yields a
   * sane, deterministic delta. */
  uint32_t t2_load = 0;          /* programmed 16-bit reload value */
  uint64_t t2_count_at_load = 0; /* CP0 Count snapshot at the (re)load */
  uint16_t t2_latch = 0;         /* value snapshotted by a latch command */
  uint8_t  t2_wr_phase = 0;      /* load: 0=expect low byte, 1=expect high byte */
  uint8_t  t2_rd_phase = 0;      /* read: 0=return low byte, 1=return high byte */
  bool     t2_loading = false;   /* an RW=both program is in progress */
  uint16_t t2_value();           /* live down-counted value, derived from CP0 Count */
public:
  sgi_hpc(state_t *s) : s(s), intstat(0), misc(0) {}
  uint32_t read(uint32_t offs, size_t sz);
  void write(uint32_t offs, uint32_t x, size_t sz);
};


#endif
