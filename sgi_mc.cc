#include "sgi_mc.hh"
#include "helper.hh"
#include "interpret.hh"
#include <cstdio>
#include <cstdlib>

/* Define ENABLE_O32_TRACE to compile in the IRIX-memory debug tracing (MCFGLOG here,
 * plus WRTRACK/DEVMAP/PTETRACK/RAMCAP in interpret.cc/sparse_mem.*). Off by default. */
#ifdef ENABLE_O32_TRACE
static const bool g_mcfglog = getenv("MCFGLOG") != nullptr;   /* read-once debug knob */
#endif

/* device-trace spew off by default; set DEVTRACE=1 to re-enable. */
static const bool dev_verbose = getenv("DEVTRACE") != nullptr;
#define DPRINTF(...) do { if(dev_verbose) fprintf(stderr, __VA_ARGS__); } while(0)

/* https://erikarn.github.io/sgi/indy/datasheets/sgi_indy_mc.pdf */

/*
All of the MC registers will respond to two different addresses. It is up
to the programmer to use the correct address depending on the endian mode of
the processor.

The MC is connected to the least significant 32 bits of the sysad bus.
When a register is written the data must be driven on those bits.
When register is read the data will be returned on those pins as well.

If the processor is running in big endian mode the odd word addresses,
(addresses that end in 4 and 0xc) are used.

When the processor is running in little endian mode the even word addresses,
(addresses that end in 0 and 8) are used.
*/

/*
 * MC (Memory Controller) register map. `offs` is the byte offset into the MC
 * register window (physical 0x1fa00000..0x1fafffff, see sgi_indy.hh). Offsets
 * below are the big-endian "odd word" aliases (ending in 4/c) that IRIX and the
 * IP22 PROM use; each register also responds at the even-word alias (-4).
 *
 * Cross-checked against: SGI Indy MC datasheet (indy_docs/mc.pdf), Linux
 * arch/mips/include/asm/sgi/mc.h, and NetBSD arch/sgimips. Names follow mc.h.
 *
 *   0x004  cpuctrl0    CPU control word 0 (refresh, parity, watchdog, endian)
 *   0x00c  cpuctrl1    CPU control word 1 (GIO timeout, HPC/EXP endianness)
 *   0x014  watchdogt   Watchdog timer (read-only; any write clears it)
 *   0x01c  systemid    System ID: [3:0]=MC revision, [4]=EPRESENT (EISA present)
 *   0x02c  divider     RPSS divider (real-time counter prescale)
 *   0x034  eeprom      Serial EEPROM byte register (bit-banged NMC93xx)
 *   0x044  rcntpre     Refresh counter preload
 *   0x04c  rcounter    Refresh counter (read-only)
 *   0x084  giopar      GIO64 arbitration / bus parameter word
 *   0x08c  cputp       CPU bus arbitration time period
 *   0x09c  lbursttp    Long-burst time period
 *   0x0c4  mconfig0    Memory config bank 0/1 (bank valid, base addr, size)
 *   0x0cc  mconfig1    Memory config bank 2/3
 *   0x0d4  cmacc       CPU memory access config
 *   0x0dc  gmacc       GIO memory access config
 *   0x0e4  cerr        CPU error address (read-only)
 *   0x0ec  cstat       CPU error status (write clears)
 *   0x0f4  gerr        GIO error address (read-only)
 *   0x0fc  gstat       GIO error status (write clears)
 *   0x104  syssembit   System semaphore (single bit, test-and-set)
 *   0x10c  mlock       GIO memory access lock
 *   0x114  elock       EISA-from-GIO access lock
 *   0x154+ gio_dma_*   GIO DMA translation / control registers
 *   0x184+ dtlb_*      GIO DMA TLB (hi/lo pairs, 4 entries)
 *   0x1dc+ dma*        GIO DMA engine (size, stride, mode, count, start, run)
 *   0x1004 rpss        RPSS 32-bit free-running counter (read-only)
 */

uint32_t sgi_mc::read(uint32_t offs, size_t sz) {
  DPRINTF("read access to MC, reg %x pc %lx\n", offs, (unsigned long)s->pc);
  uint32_t x = 0;
  switch(offs)
    {
    case 0x0:    /* cpuctrl0 (even alias) */
    case 0x4:    /* cpuctrl0 CPU control word 0 */
    case 0x8:    /* cpuctrl1 (even alias) */
    case 0xc: {  /* cpuctrl1 CPU control word 1 */
      const uint32_t index = (offs >> 3) & 1;   /* cpuctrl0(0x0/0x4) vs cpuctrl1(0x8/0xc): bit 3, not bit 1 */
      x = cpu_control[index];
      break;
    }
    case 0xc4:   /* mconfig0 memory config banks 0/1 */
    case 0xcc: { /* mconfig1 memory config banks 2/3 */
      /* mconfig0=0xc4, mconfig1=0xcc differ in bit 3, NOT bit 1 -- (offs>>1)&1 gave
       * 0 for BOTH, so mconfig1 aliased mconfig0 and IRIX saw banks 2/3 == banks 0/1
       * => 512 MB instead of 256 MB, allocating frames into the 0x1f device window. */
      const uint32_t index = (offs >> 3) & 1;
      x = memcfg[index];
#ifdef ENABLE_O32_TRACE
      if(g_mcfglog) fprintf(stderr, "[MCFG] READ  mconfig%u offs=%02x sz=%zu -> %08x\n", index, offs, sz, x);
#endif
      break;
    }
    case 0x84:   /* giopar GIO64 arbitration / bus parameter word */
      x = gio64_arb_param;
      break;
    case 0xd4:   /* cmacc CPU memory access config */
      x = cpu_mem_access_config;
      break;
    case 0xdc:   /* gmacc GIO memory access config */
      x = gio_mem_access_config;
      break;
    case 0x1c:   /* systemid */
      /* MC System ID: low nibble = revision, bit4 (EPRESENT) = EISA present.
       * Indy has no EISA, so EPRESENT stays clear. */
      x = sys_id;
      break;
    case 0x30:   /* eeprom (even alias): serial EEPROM data bit, SDATAI high */
      x = 0x10;//eeprom_ctrl & (~0x10);
      break;
    case 0x1004: /* rpss free-running counter (read-only) */
      //printf("rpss counter read\n");
      x = static_cast<uint32_t>(s->icnt/10);//rpss_counter;
      break;
    default:
      DPRINTF("trying to read MC reg %x (lenient -> 0)\n", offs);
      x = 0;
      break;
    }
  //printf("read access to MC, reg %x, value %x\n", offs, x);  
  return x;
}

uint32_t nbits = 0;
static uint8_t eerom[256] = {0};
static uint8_t byte = 0;
static uint32_t cbyte = 0;

void sgi_mc::write(uint32_t offs, uint32_t x, size_t sz) {
  DPRINTF("write access to MC, reg %x, value %x, size %lu pc %lx\n", offs, x, sz, (unsigned long)s->pc);
  
  switch(offs)
    {
    case 0x0:    /* cpuctrl0 (even alias) */
    case 0x4:    /* cpuctrl0 CPU control word 0 */
    case 0xc: {  /* cpuctrl1 CPU control word 1 */
      const uint32_t index = (offs >> 3) & 1;   /* cpuctrl0(0x0/0x4) vs cpuctrl1(0x8/0xc): bit 3, not bit 1 */
      cpu_control[index] = x;
      break;
    }
    case 0x2c:   /* divider RPSS prescale */
      rpss_divider = x;
      break;
    case 0x84:   /* giopar GIO64 arbitration / bus parameter word */
      gio64_arb_param = x;
      break;
    case 0xc4:   /* mconfig0 memory config banks 0/1 */
    case 0xcc: { /* mconfig1 memory config banks 2/3 */
      const uint32_t index = (offs >> 3) & 1;   /* bit 3 selects mconfig0 vs mconfig1 (see read) */
      memcfg[index] = x;
#ifdef ENABLE_O32_TRACE
      if(g_mcfglog) fprintf(stderr, "[MCFG] WRITE mconfig%u offs=%02x sz=%zu <- %08x\n", index, offs, sz, x);
#endif
      break;
    }
    case 0xd4:   /* cmacc CPU memory access config */
      cpu_mem_access_config = x;
      break;
    case 0xdc:   /* gmacc GIO memory access config */
      gio_mem_access_config =x;
      break;
    case 0xec:   /* cstat CPU error status (any write clears) */
      cpu_error_status = 0;
      break;
    case 0xfc:   /* gstat GIO error status (any write clears) */
      gio_error_status = 0;
      break;
    case 0x30:   /* eeprom serial EEPROM control: bit-bang CS/CLK/DATA */
      eeprom_ctrl = x;
      if ( ((x>>1) & 3) == 3) {
	DPRINTF("data bit %d, bit %u\n", (x>>3)&1, nbits);
	byte = (byte << 1) | ((x>>3)&1);
	++nbits;	
	if(nbits==8) {
	  DPRINTF("wrote byte %u : %x\n", cbyte, (int)byte);	  
	  eerom[cbyte] = byte;
	  ++cbyte;
	  nbits = 0;
	  byte = 0;

	}

      }
      break;
    default:
      DPRINTF("write to unknown MC reg %x (lenient -> ignore)\n", offs);
      break;
    }
}
