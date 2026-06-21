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
  /* IOC2/INT2 local interrupt controller (guinness/Indy): local0 status/mask at
   * IOC2 off 0x20/0x21 (abs 0x59883/0x59887), local1 at 0x22/0x23 (0x5988b/0x5988f).
   * local0 -> CPU IP2, local1 -> IP3. SCSI0 INTRQ = local0 bit 0x02 (computed live
   * from the WD33C93 intrq). */
  uint8_t ioc2_local_status[2] = {0, 0};
  uint8_t ioc2_local_mask[2]   = {0, 0};
  /* Mappable ("local2/local3", kernel's vmeistat/cmeimask) cascade: the SCC
   * serial INT is mappable bit5.  Map status = vmeistat (live); Map masks =
   * cmeimask0/1; a set bit in (vmeistat & cmeimask0) drives local0 LIO2 (bit7)
   * -> IP2 (cmeimask1 -> local1 LIO3 -> IP3).  cmepol stored, unused. */
  uint8_t ioc2_cmeimask[2] = {0, 0};
  uint8_t ioc2_cmepol      = 0;
  uint8_t ioc2_vmeistat_live();      /* live mappable status (bit5 = SCC serial INT) */
  uint8_t ioc2_local0_live();        /* local0 status incl. live SCSI0 intrq + LIO2 */
  uint32_t pbus_pio_config[10] = {0};/* reg 0x5d000 block: per-channel PIO config (10 ch) */
  uint32_t pbus_dma_config[8] = {0}; /* reg 0x5c000 block: per-channel DMA config (8 ch) */

  /* HPC3 SCSI DMA channel (HD0/HD1).  Pure-physical scatter-gather: walk the
   * {BP,BC,DP} descriptor chain via nbdp until EOX, moving bytes between DRAM
   * and the WD33C93 on each DRQ.  Control is an explicit descriptor-walk FSM. */
  enum dma_state_t { CH_IDLE, CH_FETCH, CH_XFER, CH_DESC_DONE };
  struct scsi_dma_t {
    uint32_t cbp = 0;       /* current buffer pointer (descriptor BP) */
    uint32_t nbdp = 0;      /* next-descriptor pointer (descriptor DP) */
    uint32_t bc = 0;        /* byte-count word: flags (EOX/XIE) + count */
    uint32_t ctrl = 0;      /* DMA control register */
    uint32_t count = 0;     /* live per-descriptor byte count */
    uint32_t dmacfg = 0;
    uint32_t piocfg = 0;
    bool active = false;    /* ch_active */
    bool to_device = false; /* DIR: 1 = mem->device (write) */
    dma_state_t state = CH_IDLE;
  };
  scsi_dma_t scsi_dma[2];
  void scsi_fetch_chain(int ch);   /* load {cbp,bc,nbdp,count} from nbdp */
  void scsi_run_dma(int ch);       /* pump the descriptor-walk FSM */
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
  /* true when an unmasked local0 interrupt is pending -> drive CP0 Cause IP2 */
  bool ioc2_ip2_pending() { return (ioc2_local0_live() & ioc2_local_mask[0]) != 0; }
  sgi_hpc(state_t *s) : s(s), intstat(0), misc(0) {}
  uint32_t read(uint32_t offs, size_t sz);
  void write(uint32_t offs, uint32_t x, size_t sz);
};


#endif
