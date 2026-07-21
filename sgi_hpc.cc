#include "sgi_hpc.hh"
#include "interpret.hh"
#include "sgi_scsi.hh"
#include "sgi_seeq.hh"
#include "sgi_scc.hh"
#include "cache_model.hh"
#include <cassert>
#include <cstdlib>
#include <ctime>

/* device-trace spew is off by default (it floods the console stream and slows the
 * boot to a crawl); set DEVTRACE=1 to re-enable. */
static const bool dev_verbose = getenv("DEVTRACE") != nullptr;
#define DPRINTF(...) do { if(dev_verbose) fprintf(stderr, __VA_ARGS__); } while(0)

/* RTC time source. Default: the live host wall-clock. Deterministic-time mode
 * (DETTIME env -- set on BOTH interp_mips and the JIT sim for reproducible
 * lockstep co-sim): a fixed plausible epoch advanced by retired-instruction
 * count, so the DS1286 clock is identical run-to-run instead of reading
 * ::time(nullptr). The base + rate must match the JIT sim's rtc_now exactly. */
static time_t rtc_now(state_t *s) {
  static const bool det = getenv("DETTIME") != nullptr;
  if(det) {
    static const time_t   DETTIME_BASE = 1717200000;     /* 2024-06-01 00:00:00 UTC */
    static const uint64_t DETTIME_RATE = 100000000ull;   /* ~100 M retired insns / simulated sec */
    return DETTIME_BASE + (time_t)(s->icnt / DETTIME_RATE);
  }
  return ::time(nullptr);
}

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

static inline void wr_be32(state_t *s, uint32_t pa, uint32_t v) {
  uint8_t *p = s->mem.get_raw_ptr(pa);
  p[0] = (v >> 24) & 0xff; p[1] = (v >> 16) & 0xff; p[2] = (v >> 8) & 0xff; p[3] = v & 0xff;
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
        if(d.to_device) {
          s->scsi->dma_w(*p);
        } else {
          /* SPEC_DMA: model r9999 OOO speculation -- pull this DMA-target line
           * into cache (PRE-DMA DRAM contents) before the DMA overwrites DRAM,
           * so a stale speculative copy exists that IRIX never invalidates. */
          static const bool g_spec = getenv("SPEC_DMA") != nullptr;
          if(g_spec && g_cmodel) { g_cmodel->spec_fill((uint32_t)d.cbp); }
          *p = s->scsi->dma_r();                                   /* DMA -> DRAM (post-DMA) */
          /* COHERENT_DMA: the fix -- snoop-invalidate the written line so the
           * stale speculative copy cannot survive to the next CPU read. */
          static const bool g_coh = getenv("COHERENT_DMA") != nullptr;
          if(g_coh && g_cmodel) { g_cmodel->snoop_inval((uint32_t)d.cbp); }
        }
        // WATCHLINE: at the DMA write of the crash line, is that line currently
        // RESIDENT in the CPU cache?  resident => the CPU architecturally cached
        // it before this DMA (page-recycling, reproducible in-order); never
        // resident => it can only get stale via OOO speculation (unreachable here).
        { static const bool g_wl = getenv("WATCHLINE") != nullptr;
          if(g_wl && !d.to_device && g_cmodel &&
             ((uint32_t)d.cbp & ~(cache_model::L1_LINE - 1)) == cache_model::WATCH_LINE) {
            cache_model::presence pr = g_cmodel->probe((uint32_t)d.cbp);
            fprintf(stderr, "[wl-dma] icnt=%llu pa=%08x wrote=%02x resident=%d dirty=%d %s\n",
                    (unsigned long long)s->icnt, (uint32_t)d.cbp, *p, pr.resident, pr.dirty,
                    pr.resident ? "(RECYCLING: cached before DMA)" : "(not cached -> needs speculation)"); }
        }
        // WATCHDMA: catch the DMA landing the /sbin/sh .data pointer 0x0e07bbc0 (big-endian
        // 0e 07 bb c0) into DRAM -> pins physical(0x0e0981c4) + confirms it is DMA-paged-in.
        { static const bool g_wdma = getenv("WATCHDMA") != nullptr;
          if(g_wdma && !d.to_device && *p == 0xc0u && d.cbp >= 3) {
            uint8_t *q = s->mem.get_raw_ptr(d.cbp - 3);
            if(q[0]==0x0eu && q[1]==0x07u && q[2]==0xbbu && q[3]==0xc0u)
              fprintf(stderr, "[dma-wr-c0] pa=%09llx icnt=%llu\n",
                      (unsigned long long)(d.cbp - 3), (unsigned long long)s->icnt); }
        }
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

/* HPC3 ENET descriptor-walk against the Seeq 8003 (ch0 = RX device->mem,
 * ch1 = TX mem->device).  Mirrors scsi_run_dma; the Seeq replaces the WD33C93.
 * TX drains the whole chain into a frame then transmits at EOX; RX writes a
 * staged [pad][frame][status] byte stream into the buffer until it's consumed. */
/* HPCDMA descriptor cntinfo bits (asm/sgi/hpc3.h) */
static const uint32_t HPCDMA_EOX  = 0x80000000u;  /* tx: end of chain */
static const uint32_t HPCDMA_EOR  = 0x80000000u;  /* rx: end of ring */
static const uint32_t HPCDMA_XIE  = 0x20000000u;  /* irq at end of this desc */
static const uint32_t HPCDMA_ETXD = 0x00008000u;  /* set by HPC when packet tx'd */
static const uint32_t HPCDMA_OWN  = 0x00004000u;  /* rx: HPC owns the buffer */
static const uint32_t HPCDMA_BCNT = 0x00003fffu;  /* buffer byte count */

/* Drive one ENET DMA channel against the sgiseeq descriptor rings. Channel 1 is
 * TX (mem->Seeq->tap); channel 0 is RX (tap->Seeq->mem). Each descriptor is
 * {pbuf, cntinfo, pnext} in guest DRAM (big-endian, read via rd_be32). */
void sgi_hpc::enet_run_dma(int ch) {
  scsi_dma_t &d = enet_dma[ch];
  if(!s->seeq) { return; }
  if(ch == 1) {
    /* TX: walk the chain from tx_ndptr, transmit each buffer, mark ETXD so the
     * driver's sgiseeq_tx acks it; XIE raises the TX-channel irq. */
    while(d.active) {
      uint32_t desc    = d.nbdp;
      uint32_t pbuf    = rd_be32(s, desc);
      uint32_t cntinfo = rd_be32(s, desc + 4);
      uint32_t pnext   = rd_be32(s, desc + 8);
      uint32_t cnt     = cntinfo & HPCDMA_BCNT;
      for(uint32_t i = 0; i < cnt; i++) {
        uint8_t *p = s->mem.get_raw_ptr(pbuf + i);
        s->seeq->tx_dma_w(*p);
      }
      wr_be32(s, desc + 4, cntinfo | HPCDMA_ETXD);      /* HPC transmitted this buffer */
      if(cntinfo & HPCDMA_XIE) { intstat |= 0x800u; }   /* XIE -> TX chan irq (istat bit 11) */
      d.cbp  = pbuf;
      d.nbdp = pnext;
      /* Flush ONLY at end-of-chain: IRIX's if_ec scatter-gathers a frame across
       * several descriptors (e.g. ethernet header in one buffer, payload in the
       * next), so accumulate the whole chain into one frame and transmit it once.
       * (Flushing per-buffer split each descriptor into its own bogus frame -- worked
       * for Linux sgiseeq only because it puts the whole frame in one buffer.) */
      if(cntinfo & HPCDMA_EOX) {
        s->seeq->tx_flush();                            /* whole frame assembled -> out the tap */
        d.active = false;                               /* end of chain */
      }
    }
    d.ctrl &= ~0x200u;                                  /* ETXCTRL_ACTIVE clears when idle */
  }
  else {
    /* RX: ring of HPC-owned descriptors. For each staged frame, fill the current
     * descriptor's buffer with [pad][pad][frame][status], write back cntinfo with
     * OWN cleared and BCNT=residual (driver: len = PKT_BUF_SZ - residual - 3). */
    while(d.active and s->seeq->rx_avail()) {
      uint32_t desc    = d.nbdp;
      uint32_t cntinfo = rd_be32(s, desc + 4);
      if(!(cntinfo & HPCDMA_OWN)) { break; }            /* CPU still owns it: stall (retry next poll) */
      uint32_t pbuf    = rd_be32(s, desc);
      uint32_t pnext   = rd_be32(s, desc + 8);
      uint32_t bufsz   = cntinfo & HPCDMA_BCNT;         /* PKT_BUF_SZ */
      uint32_t written = 0;
      bool last = false;
      while(written < bufsz and s->seeq->rx_avail()) {
        uint8_t *p = s->mem.get_raw_ptr(pbuf + written);
        *p = s->seeq->rx_dma_r(last);
        written++;
        if(last) { break; }                             /* [pad][pad][frame][status] complete */
      }
      uint32_t residual = bufsz - written;
      wr_be32(s, desc + 4, (cntinfo & ~HPCDMA_OWN & ~HPCDMA_BCNT) | (residual & HPCDMA_BCNT));
      if(cntinfo & HPCDMA_XIE) { intstat |= 0x400u; }   /* XIE -> RX chan irq (istat bit 10) */
      {
        static const bool g_ed = getenv("ENET_DBG") != nullptr;
        if(g_ed) fprintf(stderr, "ENET RX-DMA: wrote %u B to pbuf=%08x desc=%08x XIE=%d intstat=%08x mask0=%02x local0=%02x\n",
                         written, pbuf, desc, (cntinfo & HPCDMA_XIE) != 0, intstat, ioc2_local_mask[0], ioc2_local0_live());
      }
      d.cbp  = pbuf;
      d.nbdp = pnext;                                   /* advance around the ring */
      if(cntinfo & HPCDMA_EOR) { break; }               /* end of ring: HPC pauses, driver re-arms */
    }
  }
}

/* Per-tick hook (called from maybe_take_interrupt): pull inbound frames off the
 * tap into the Seeq, then run the RX DMA if the RX channel is armed. */
void sgi_hpc::enet_poll() {
  if(!s->seeq) { return; }
  /* poll() reads the tap fd -- a syscall. Called per-instruction it dominates the
   * profile (libc read + tun_chr_read_iter), so throttle to 1-in-1024. RX latency
   * of ~1024 insns is negligible vs. the ms-scale network; the tap's own kernel
   * buffer holds frames between polls. */
  if((++enet_poll_ctr & 0x3ffu) != 0) { return; }
  s->seeq->poll();
  if(enet_dma[0].active) { enet_run_dma(0); }
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
  /* ENET (bit3): the CPU interrupt is the HPC3 DMA-channel XIE latch (intstat bits
   * 10/11), cleared by the ENET reset reg CLRIRQ. The Seeq's own INTRQ is consumed
   * by the HPC internally (per-frame DMA'd status), NOT routed to IP2 -- folding it
   * here caused an interrupt storm since the driver never reads the Seeq rstat reg
   * to clear it. */
  if(intstat & 0x00000c00u) st |= 0x08u;
  else                      st &= ~0x08u;
  if((ioc2_vmeistat_live() & ioc2_cmeimask[0]) != 0) st |= 0x80u;   /* LIO2 */
  else                                               st &= ~0x80u;
  return st;
}

uint32_t sgi_hpc::read(uint32_t offs, size_t sz) {
  DPRINTF("%s at pc %x : %x unimplemented\n", __PRETTY_FUNCTION__, s->pc, offs);
  static const bool g_enet_dbg = getenv("ENET_DBG") != nullptr;
  if(g_enet_dbg and ((offs >= 0x14000 and offs <= 0x17fff) or
                     (offs >= 0x54000 and offs <= 0x5401f) or
                     (offs >= 0x60000 and offs <= 0x67fff))) {
    fprintf(stderr, "ENET-R pc=%08x offs=%05x sz=%zu\n", s->pc, offs, sz);
  }
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
  else if(offs >= 0x00014000 and offs <= 0x0001ffff) { /* ENET RX(0x14000)/TX(0x16000) DMA regs */
    int ch = (offs & 0x2000) ? 1 : 0;
    uint32_t n = offs & ~0x2000u;
    scsi_dma_t &d = enet_dma[ch];
    uint32_t r = 0;
    switch(n) {
    case 0x14000: r = d.cbp; break;
    case 0x14004: r = d.nbdp; break;
    case 0x15000: r = (d.count & 0x3fff) | (d.bc & ~0x3fffu); break;
    case 0x15004:                                       /* rx_ctrl/tx_ctrl: ACTIVE + seeq status */
      r = (d.ctrl & 0x200u);                            /* HPC3_E*XCTRL_ACTIVE reflects channel state */
      if(s->seeq) { r |= (ch == 0) ? s->seeq->rstat_peek() : s->seeq->tstat_peek(); }
      break;
    case 0x15014:                                       /* reset reg read: CLRIRQ(0x2) reflects a
                                                         * pending ENET DMA-channel irq. IRIX's ec ISR
                                                         * reads this, and only writes CLRIRQ if bit 0x2
                                                         * is set -- without it the irq never clears -> storm. */
      r = (intstat & 0x00000c00u) ? 0x2u : 0x0u;
      break;
    case 0x15018: r = d.dmacfg; break;
    default:      r = 0; break;
    }
    return __builtin_bswap32(r);
  }
  else if(offs >= 0x00054000 and offs <= 0x0005401f) {
    /* Seeq 8003/80C03 ENET device regs: 8 byte-registers, word-spaced (reg N at
     * +N*4). Model the Indy as "ethernet present, cable unplugged": reg0
     * collision counter = 0 (the ec driver reads this as 0 to identify the SGI
     * 80C03 EDLC, i.e. controller present), reg5 flags = NO_CARRIER, and the
     * rx/tx status regs report OLD (stale, nothing pending). No packets are
     * ever delivered, so a process polling the interface stops blocking once it
     * sees "no carrier" instead of waiting forever on a controller-less ISS. */
    uint32_t idx = ((offs - 0x54000) >> 2) & 7;
    uint8_t v = s->seeq ? s->seeq->pio_r(idx) : 0;
    return (sz == 1) ? v : ((uint32_t)v << 24);
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
    /* HENNY-MATCH: under DETTIME, return the henny FPGA RTL's FROZEN ds1386 date
     * (rtl/hpc3.sv: 2030-01-01 00:00:00, dow=1) bit-exactly, so the golden ISS takes
     * the SAME date-dependent reconfigure path as silicon. IRIX rtodc() (IP22.c:3617-3644)
     * decodes the BCD year as year = 1940 + bcd, with a pivot: bcd<45 adds 30 (so bcd
     * 00-44 -> 1970-2014, bcd 45-99 -> 1985-2039). BCD 0x90 = 90 >= 45 -> 1940+90 = 2030.
     * 2030 is deliberately AFTER the clean image's ~2026-06 /var/sysgen mtimes, so a
     * reconfigure writes /unix with mtime > sysgen and IRIX reconfigures ONCE, then skips
     * it on every later boot -- no disk backdating needed. month/date default 1. */
    static const bool g_det = getenv("DETTIME") != nullptr;
    if(g_det) {
      uint8_t hv;
      switch(offs) {
      case 0x60018: hv = 0x01; break;  /* day-of-week = 1 */
      case 0x60020: hv = 0x01; break;  /* date = 1st */
      case 0x60024: hv = 0x01; break;  /* month = January */
      case 0x60028: hv = 0x90; break;  /* year BCD 90 -> IRIX rtodc 1940+90 = 2030 (matches hpc3.sv) */
      default:      hv = 0x00; break;  /* sec/min/hour/cmd = 0 (BCD 00) */
      }
      return (uint32_t)hv << 24;
    }
    time_t now = rtc_now(s);
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
  else if(offs >= 0x000604e8 and offs <= 0x000604fc and (offs & 3) == 0) {
    /* IP22 station ethernet address in the ds1386 bbRAM at word 250..255
     * (ip22-nvram.c: ip22_nvram_read, EADDR_NVOFS=250, Indy/Guiness path reads
     * hpc3c0->bbram[reg*2 + {0,1}]; bbram base is 0x60100, so reg 250 -> 0x604e8).
     * Real SGI OUI 08:00:69 so the sgiseeq driver gets a valid MAC instead of the
     * all-zero address (which fails is_valid_ether_addr).  Byte in [31:24]; the BE
     * load path bswaps device reads to the kernel's low byte (like SYSID/RTC). */
    static const uint8_t mac[6] = {0x08, 0x00, 0x69, 0x12, 0x34, 0x56};
    uint32_t i = (offs - 0x000604e8) >> 2;   /* 0..5 */
    return (uint32_t)mac[i] << 24;
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
  static const bool g_enet_dbg = getenv("ENET_DBG") != nullptr;
  if(g_enet_dbg and ((offs >= 0x14000 and offs <= 0x17fff) or
                     (offs >= 0x54000 and offs <= 0x5401f) or
                     (offs >= 0x60000 and offs <= 0x67fff))) {
    fprintf(stderr, "ENET-W pc=%08x offs=%05x val=%08x sz=%zu\n", s->pc, offs, x, sz);
  }
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
  else if(offs >= 0x00014000 and offs <= 0x0001ffff) { /* ENET RX(0x14000)/TX(0x16000) DMA regs */
    int ch = (offs & 0x2000) ? 1 : 0;
    uint32_t n = offs & ~0x2000u;
    scsi_dma_t &d = enet_dma[ch];
    x = __builtin_bswap32(x);
    switch(n) {
    case 0x14000: d.cbp    = x; break;
    case 0x14004: d.nbdp   = x; break;
    case 0x15000: d.bc     = x; break;
    case 0x15018: d.dmacfg = x; break;
    case 0x15004: {                                   /* rx_ctrl(ch0)/tx_ctrl(ch1): ACTIVE = 0x200 */
      bool was_active = d.active;
      d.ctrl   = x;
      d.active = (x & 0x200u) != 0;                    /* HPC3_ERXCTRL/ETXCTRL_ACTIVE */
      if(d.active and not was_active) { enet_run_dma(ch); }  /* newly armed: TX runs now, RX waits on frames */
      break;
    }
    case 0x15014:                                     /* ENET reset (RX region): CRESET|CLRIRQ */
      if(x & 0x2u) { intstat &= ~0x00000c00u; }        /* CLRIRQ: clear RX(0x400)+TX(0x800) chan irq */
      if(x & 0x1u) {                                   /* CRESET: reset both ENET DMA channels */
        enet_dma[0].active = false; enet_dma[0].ctrl = 0;
        enet_dma[1].active = false; enet_dma[1].ctrl = 0;
        intstat &= ~0x00000c00u;
        if(s->seeq) { s->seeq->reset(); }
      }
      break;
    default: break;
    }
    return;
  }
  else if(offs >= 0x00054000 and offs <= 0x0005401f) { /* Seeq 8003 ENET device regs */
    /* Byte registers, word-spaced. On a word store (sz==4) the BE MMIO path puts
     * the byte in bits [31:24] (symmetric with the read handler's v<<24); a byte
     * store carries it in [7:0]. Extracting (uint8_t)x unconditionally zeroed
     * rx_cmd + the station MAC -> receiver disabled. */
    uint8_t b = (sz == 1) ? (uint8_t)x : (uint8_t)(x >> 24);
    if(s->seeq) { s->seeq->pio_w(((offs - 0x54000) >> 2) & 7, b); }
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
