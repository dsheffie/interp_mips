#include "sgi_hpc.hh"
#include "interpret.hh"
#include "sgi_scsi.hh"
#include "sgi_scc.hh"
#include <cassert>
#include <cstdlib>
#include <ctime>

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
  static const bool dbg = getenv("SCSIDBG") != nullptr;
  if(dbg) fprintf(stderr, "sgi_hpc: ch%d FETCH desc=%08x cbp=%08x bc=%08x count=%u next=%08x %s%s\n",
                  ch, desc, d.cbp, d.bc, d.count, d.nbdp,
                  (d.bc & 0x80000000u) ? "EOX " : "", (d.bc & 0x20000000u) ? "XIE" : "");
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
        /* Chunked transfer: IRIX programs the WD33C93 transfer count (and this
         * DMA chain) for fewer bytes than the SCSI command's full length, then
         * resumes with a fresh chain + SEL_ATN_XFER. If the device data-in buffer
         * isn't fully drained when the chain ends (EOX), post a transfer-paused
         * interrupt so IRIX continues, rather than stalling with no INTRQ. */
        if(s->scsi && s->scsi->residual() > 0) s->scsi->pause_transfer();
        d.active = false; d.ctrl &= ~0x10u; d.state = CH_IDLE;
      } else {
        d.state = CH_FETCH;
      }
      progress = true;
      break;
    }
  }
}

/* Live mappable interrupt status (kernel's vmeistat).  bit5 = SCC serial INT
 * (the Z8530 INT line: Tx-buffer-empty, and Rx when modeled). */
uint8_t sgi_hpc::ioc2_vmeistat_live() {
  uint8_t v = 0;
  if(s->scc && s->scc->int_pending()) v |= 0x20u;   /* bit5 = SERIAL DUART */
  return v;
}

/* local0 interrupt status with the live SCSI0 INTRQ bit (0x02) folded in from the
 * WD33C93 (level-sensitive: reflects intrq, cleared when IRIX reads SCSI Status),
 * plus LIO2 (bit7) = OR of the mappable cascade (vmeistat & cmeimask0) -- this is
 * how the SCC serial INT reaches IP2. */
uint8_t sgi_hpc::ioc2_local0_live() {
  uint8_t st = ioc2_local_status[0];
  if(s->scsi && s->scsi->intrq_pending()) st |= 0x02u;
  else                                    st &= ~0x02u;
  if((ioc2_vmeistat_live() & ioc2_cmeimask[0]) != 0) st |= 0x80u;   /* LIO2 */
  else                                               st &= ~0x80u;
  return st;
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
    uint32_t r = 0;                                      /* 32-bit regs: BE load path will swap the result */
    switch(n) {
    case 0x10000: r = d.cbp; break;
    case 0x10004: r = d.nbdp; break;
    case 0x11000: r = (d.count & 0x3fff) | (d.bc & ~0x3fffu); break;
    case 0x11004:                                        /* ctrl: read clears the IRQ bit */
      r = d.ctrl;
      if(intstat & (0x100u << ch)) { r |= 0x01u; intstat &= ~(0x100u << ch); }
      break;
    case 0x11010: r = d.dmacfg; break;
    case 0x11014: r = d.piocfg; break;
    default:      r = 0; break;                          /* gio/dev fifo ptr */
    }
    return __builtin_bswap32(r);
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
  /* IOC2/INT2 local interrupt status/mask (byte regs at IOC2 off N -> 0x59800+N*4+3) */
  else if(offs == 0x59883) { return ioc2_local0_live(); }   /* local0 status */
  else if(offs == 0x59887) { return ioc2_local_mask[0]; }   /* local0 mask   */
  else if(offs == 0x5988b) { return ioc2_local_status[1]; } /* local1 status */
  else if(offs == 0x5988f) { return ioc2_local_mask[1]; }   /* local1 mask   */
  else if(offs == 0x59893) { return ioc2_vmeistat_live(); } /* map status (vmeistat) */
  else if(offs == 0x59897) { return ioc2_cmeimask[0]; }     /* map mask0 (cmeimask0) */
  else if(offs == 0x5989b) { return ioc2_cmeimask[1]; }     /* map mask1 (cmeimask1) */
  else if(offs == 0x5989f) { return ioc2_cmepol; }          /* map polarity (cmepol) */
  /* DS1286/DS1386 RTC in the bbRAM window (byte-per-word x4: reg N at 0x60000+N*4).
   * Return the live host wall-clock in BCD so IRIX boots with a sane date instead
   * of 1970/2000 -- a bogus clock makes the man-page index (makewhatis/sgindex)
   * look perpetually stale, triggering a full rebuild on every boot. Value in
   * byte[31:24] -- the BE load path bswaps device reads (like SYSID at 0x59858).
   * localtime() is read fresh per access; the few reads of a full clock snapshot
   * land within microseconds of each other, so the fields stay coherent. */
  else if(offs >= 0x60000 and offs <= 0x6002f) {
    time_t now = ::time(nullptr);
    struct tm lt; localtime_r(&now, &lt);
    auto bcd = [](int n) -> uint8_t { return (uint8_t)(((n / 10) << 4) | (n % 10)); };
    uint8_t v;
    switch(offs) {
    case 0x60004: v = bcd(lt.tm_sec);        break;  /* seconds (reg 1) */
    case 0x60008: v = bcd(lt.tm_min);        break;  /* minutes (reg 2) */
    case 0x60010: v = bcd(lt.tm_hour);       break;  /* hours   (reg 4, 24h BCD) */
    case 0x60018: v = bcd(lt.tm_wday + 1);   break;  /* day-of-week (reg 6, 1-7) */
    case 0x60020: v = bcd(lt.tm_mday);       break;  /* date    (reg 8, 1-31) */
    case 0x60024: v = bcd(lt.tm_mon + 1);    break;  /* month   (reg 9, 1-12) */
    case 0x60028: v = bcd(lt.tm_year % 100); break;  /* year    (reg 10, BCD 2-digit) */
    default:      v = 0x00;                  break;  /* command/alarm/hundredths -> not-busy */
    }
    return (uint32_t)v << 24;
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
    /* These are 32-bit registers; the BE store path delivers the word byte-
     * swapped to MMIO handlers (byte accesses like the WD33C93 are unaffected).
     * Swap back to the guest's intended value. */
    x = __builtin_bswap32(x);
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
  /* IOC2/INT2 local interrupt masks (byte regs); status regs are read-only */
  else if(offs == 0x59887) { ioc2_local_mask[0] = (uint8_t)x; }
  else if(offs == 0x5988f) { ioc2_local_mask[1] = (uint8_t)x; }
  else if(offs == 0x59897) { ioc2_cmeimask[0] = (uint8_t)x; } /* map mask0 (cmeimask0) */
  else if(offs == 0x5989b) { ioc2_cmeimask[1] = (uint8_t)x; } /* map mask1 (cmeimask1) */
  else if(offs == 0x5989f) { ioc2_cmepol     = (uint8_t)x; } /* map polarity (cmepol)  */
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
