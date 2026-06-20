#include "sgi_hpc.hh"
#include "interpret.hh"
#include "sgi_scsi.hh"
#include <cassert>
#include <cstdlib>

/* device-trace spew is off by default (it floods the console stream and slows the
 * boot to a crawl); set DEVTRACE=1 to re-enable. */
static const bool dev_verbose = getenv("DEVTRACE") != nullptr;
#define DPRINTF(...) do { if(dev_verbose) printf(__VA_ARGS__); } while(0)

/*
 * HPC3 (High Performance Peripheral Controller, 3rd gen) register map.
 *
 * The Indy has two HPC3 chips; the "first" one (which carries the boot PROM,
 * SCSI, ethernet, serial EEPROM and PBUS peripherals) is based at physical
 * 0x1fb80000. `offs` here is the byte offset into that window, i.e.
 * pa - 0x1fb80000 (see sgi_indy.hh hpc_regs range).
 *
 * Region map (offset = spec address - 0x1fb80000). Cross-checked against the
 * HPC3 chip spec (indy_docs/hpc3.pdf), Linux arch/mips/include/asm/sgi/hpc3.h,
 * and NetBSD arch/sgimips/hpc/hpcreg.h:
 *
 *   0x00000..0x0ffff  PBUS DMA channel registers (8 channels: bp/dp/ctrl)
 *   0x10000..0x1ffff  HD0/HD1/ENET DMA channel registers
 *   0x20000..0x2ffff  FIFO access ports
 *   0x30000..0x3ffff  General registers (see below)
 *   0x40000..0x47fff  HD0 (SCSI channel 0, WD33C93) device registers
 *   0x48000..0x4ffff  HD1 (SCSI channel 1) device registers
 *   0x50000..0x57fff  ENET (Seeq 8003) device registers
 *   0x58000..0x5ffff  PBUS device registers (PIO data + DMA/PIO config)
 *   0x60000..0x7ffff  Battery-backed SRAM (NVRAM)
 *
 * General registers (0x30000 block), per NetBSD/Linux ordering:
 *   0x30000  istat0    interrupt status, bits [4:0] reliable
 *   0x30004  gio_misc  GIO misc control ("misc")
 *   0x3000b  eeprom    serial EEPROM data register (byte; NMC93CS56)
 *   0x3000c  istat1    interrupt status, bits [9:5] reliable
 *   0x30010  bestat    GIO64 bus-error interrupt status
 *
 * PBUS device sub-decode within 0x58000..0x5ffff (id = (offs>>8)&mask):
 *   0x58000..0x5bfff  per-channel PIO data ports
 *   0x5c000..0x5cfff  PBUS DMA channel config (8 channels)
 *   0x5d000..0x5dfff  PBUS PIO channel config (10 channels)
 */

/* i8254 counter 2 counts DOWN at the chip's 1 MHz against CP0 Count (which we
 * tick once per retired insn).  dosample() (ip22-time.c) derives the CPU clock
 * from Delta(c0_count) / (counter2 ticks at SGINT_TIMER_CLOCK=1MHz), so the
 * reported mips_hpt_frequency = 1e6 * RATIO and reported CPU = 2 * RATIO MHz.
 * RATIO=50 -> 100 MHz, matching the Ultra96 r9999 FPGA core clock. */
static const uint32_t T2_RATIO = 50;
uint16_t sgi_hpc::t2_value() {
  uint32_t elapsed = (uint32_t)(s->cpr0[CPR0_COUNT] - t2_count_at_load);
  uint32_t dec = elapsed / T2_RATIO;
  if(dec >= t2_load) return 0;       /* counted down to (or past) zero */
  return (uint16_t)(t2_load - dec);
}

/* read a big-endian 32-bit word from physical DRAM (descriptors are stored in
 * guest big-endian byte order; sparse_mem holds raw guest bytes). */
static inline uint32_t rd_be32(state_t *s, uint32_t pa) {
  uint8_t *p = s->mem.get_raw_ptr(pa);
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

void sgi_hpc::scsi_fetch_chain(int ch) {
  scsi_dma_t &d = scsi_dma[ch];
  uint32_t desc = d.nbdp;
  d.cbp   = rd_be32(s, desc);
  d.bc    = rd_be32(s, desc + 4);
  d.nbdp  = rd_be32(s, desc + 8);
  d.count = d.bc & 0x3fff;
}

/* Descriptor-walk FSM, pumped while it can make progress.  CH_XFER drains the
 * current descriptor against the WD33C93 byte-port until count==0 or DRQ drops
 * (data exhausted on the device side); CH_DESC_DONE handles XIE/EOX and either
 * advances to the next descriptor or deactivates the channel. */
void sgi_hpc::scsi_run_dma(int ch) {
  scsi_dma_t &d = scsi_dma[ch];
  bool progress = true;
  while(d.active && progress) {
    progress = false;
    switch(d.state) {
    case CH_IDLE:
      break;
    case CH_FETCH:
      scsi_fetch_chain(ch);
      d.state = CH_XFER;
      progress = true;
      break;
    case CH_XFER:
      while(d.count != 0 && s->scsi && s->scsi->drq_pending()) {
        uint8_t *p = s->mem.get_raw_ptr(d.cbp);
        if(d.to_device) s->scsi->dma_w(*p);
        else            *p = s->scsi->dma_r();
        d.cbp++; d.count--;
      }
      if(d.count == 0) { d.state = CH_DESC_DONE; progress = true; }
      break;  /* else DRQ dropped mid-descriptor: wait for the next DRQ */
    case CH_DESC_DONE:
      if(d.bc & 0x20000000u) intstat |= (0x100u << ch);    /* XIE -> SCSI channel IRQ */
      if(d.bc & 0x80000000u) {                              /* EOX -> deactivate */
        d.active = false; d.ctrl &= ~0x10u; d.state = CH_IDLE;
      } else {
        d.state = CH_FETCH;
      }
      progress = true;
      break;
    }
  }
}

uint32_t sgi_hpc::read(uint32_t offs, size_t sz) {
  DPRINTF("%s at pc %x : %x unimplemented\n", __PRETTY_FUNCTION__, s->pc, offs);
  if(offs == 0x598bb) {           /* i8254 counter 2: return latched low then high byte */
    uint8_t b = (t2_rd_phase == 0) ? (uint8_t)(t2_latch & 0xff)
                                   : (uint8_t)((t2_latch >> 8) & 0xff);
    t2_rd_phase ^= 1;
    return b;
  }

  if(offs <= 0x0000ffff) {        /* PBUS DMA channel registers */
    DPRINTF("pbus dma read\n");
  }
  else if(offs >= 0x00010000 and offs <= 0x00013fff) { /* SCSI HD0/HD1 DMA channel regs */
    int ch = (offs & 0x2000) ? 1 : 0;
    uint32_t n = offs & ~0x2000u;
    scsi_dma_t &d = scsi_dma[ch];
    switch(n) {
    case 0x10000: return d.cbp;
    case 0x10004: return d.nbdp;
    case 0x11000: return (d.count & 0x3fff) | (d.bc & ~0x3fffu);
    case 0x11004: {                                      /* ctrl: read clears the IRQ bit */
      uint32_t r = d.ctrl;
      if(intstat & (0x100u << ch)) { r |= 0x01u; intstat &= ~(0x100u << ch); }
      return r;
    }
    case 0x11010: return d.dmacfg;
    case 0x11014: return d.piocfg;
    default:      return 0;                              /* gio/dev fifo ptr */
    }
  }
  else if(offs >= 0x00014000 and offs <= 0x0001ffff) { /* ENET DMA regs */
    DPRINTF("enet read\n");
  }
  else if(offs >= 0x00040000 and offs <= 0x00047fff) { /* WD33C93 HD0 (SASR=+3/SCMD=+7) */
    /* HPC3 map (MAME hpc3.cpp): HD0 SCSI0 = 0x40000-0x47fff, HD1 = 0x48000-0x4ffff.
     * NOT 0x44000 -- that came from MAME's buggy unhandled-access log string. */
    if(s->scsi) return s->scsi->pio_r(((offs - 0x40000) >> 2) & 1);
    return 0;
  }
  else if(offs == 0x30000) {      /* istat0: interrupt status [4:0] */
    return intstat;
  }
  else if(offs == 0x30004) {      /* gio_misc */
    return misc;
  }
  else if(offs == 0x59858) {      /* IOC2 System ID register (phys 0x1fbd9858) */
    /* 0x26 = guinness/Indy board id; getsysid uses bits [7:5] (->001) + bit 0
     * to pick the system type. (MAME co-sim, MAME_QUESTIONS.md Q5 round-3.)
     * Return it pre-byte-swapped (like the MC mconfig regs): the load path
     * bswaps device reads, so the kernel sees 0x26 in byte [7:0]. */
    return 0x26000000u;
  }
  else if(offs >= 0x00058000 and offs <= 0x0005bfff) { /* PBUS PIO data ports */
    int id = ((offs>>8) & 0x7f)>>2;
    DPRINTF("pio data on channel %u\n", id);
  }
  //else {

  //assert(false);
    //}

  //exit(-1);
  return 0;
}

void sgi_hpc::write(uint32_t offs, uint32_t x, size_t sz) {
  //assert(sz == 4);
  if(offs == 0x598bf) {           /* i8254 control word (tcword) */
    if(((x >> 4) & 0x3) == 0) {   /* RW=00: counter-latch command -> snapshot live value */
      t2_latch = t2_value();
      t2_rd_phase = 0;
    }
    else {                        /* RW=both: program (or stop) -> expect a 2-byte reload */
      t2_loading = true;
      t2_wr_phase = 0;
    }
    return;
  }
  else if(offs == 0x598bb) {      /* i8254 counter 2 (tcnt2): low byte then high byte */
    if(t2_loading) {
      if(t2_wr_phase == 0) {
        t2_load = (t2_load & 0xff00) | (x & 0xff);
        t2_wr_phase = 1;
      }
      else {
        t2_load = (t2_load & 0x00ff) | ((x & 0xff) << 8);
        t2_wr_phase = 0;
        t2_loading = false;
        t2_count_at_load = s->cpr0[CPR0_COUNT];   /* (re)start the down-count */
      }
    }
    return;
  }
  if(offs <= 0x0000ffff) {        /* PBUS DMA channel registers */
    DPRINTF("pbus dma write\n");
  }
  else if(offs >= 0x00010000 and offs <= 0x00013fff) { /* SCSI HD0/HD1 DMA channel regs */
    int ch = (offs & 0x2000) ? 1 : 0;
    uint32_t n = offs & ~0x2000u;
    scsi_dma_t &d = scsi_dma[ch];
    switch(n) {
    case 0x10004: d.nbdp   = x; break;
    case 0x11000: d.bc     = x; break;
    case 0x11010: d.dmacfg = x; break;
    case 0x11014: d.piocfg = x; break;
    case 0x11004: {                                   /* ctrl: arm / flush / reset the channel */
      bool was_active = d.active;
      if(x & 0x20u) {                                 /* AMASK: write-protect ch_active */
        d.ctrl = x & ~0x01u & ~0x10u & ~0x20u;
        if(was_active) d.ctrl |= 0x10u;
      } else {
        d.ctrl = x & ~0x01u & ~0x20u;
        d.active = (d.ctrl & 0x10u);
      }
      d.to_device = (d.ctrl & 0x04u);                 /* DIR: 1 = mem->device */
      if(!was_active && d.active) d.state = CH_FETCH;  /* arm -> start the FSM */
      if((x & 0x40u) && s->scsi) s->scsi->reset();    /* CRESET */
      if(x & 0x08u) { d.active = false; d.ctrl &= ~0x10u; d.state = CH_IDLE; }  /* FLUSH -> stop */
      if(d.active) scsi_run_dma(ch);
      break;
    }
    default: break;
    }
    return;
  }
  else if(offs >= 0x00014000 and offs <= 0x0001ffff) { /* ENET DMA regs */
    DPRINTF("enet write\n");
    return;
  }
  else if(offs == 0x30004) {      /* gio_misc */
    misc = x&3;
  }
  else if(offs >= 0x40000 and offs <= 0x47fff) { /* WD33C93 HD0 (SASR=+3/SCMD=+7) */
    if(s->scsi) {
      s->scsi->pio_w(((offs - 0x40000) >> 2) & 1, (uint8_t)x);
      /* a COMMAND-register write may assert DRQ; service it if the channel is armed */
      if(scsi_dma[0].active && s->scsi->drq_pending()) scsi_run_dma(0);
    }
  }
  else if(offs >= 0x00058000 and offs <= 0x0005bfff) { /* PBUS PIO data ports */
    int id = ((offs>>8) & 0x7f)>>2;
    DPRINTF("pio write data on channel %x for offset %x with data %x\n", id, offs, x);
  }
  else if(offs >= 0x5c000 and offs <= 0x5cfff) { /* PBUS DMA channel config */
    int id = ((offs>>8) & 0xf)>>1;
    DPRINTF("pbus dma write for channel %d = %x\n", id, x);
    pbus_dma_config[id] = x;
    return;
  }
  else if(offs >= 0x5d000 and offs <= 0x5dfff) {
    /* pbus pio channel configuration register */
    int id = (offs>>8) & 0xf;
    DPRINTF("pio channel config %d = %x\n", id, x);
    if(id < 10) {
      pbus_pio_config[id] = x;
    }
    else {
      assert(false);
    }
    return;
  }
  else {
    DPRINTF("%s at pc %x : %x unimplemented (lenient -> ignore)\n", __PRETTY_FUNCTION__, s->pc, offs);
  }
}
