/* Checkpoint save/restore for interp_mips, modeled on rv64core's saveState.cc.
 *
 * Dumps the full architectural state (PC, GPRs, HI/LO, CP0, FPRs, FCR) plus
 * every non-zero 4 KB page of the (flat, demand-paged) sparse_mem, so an RTL
 * model (Verilator) can resume mid-workload without re-running the boot. The
 * RTL seeds its register files from this via DPI (loadgpr/loadfpr/loadcp0) and
 * resumes at the checkpoint PC -- see the henry sim tb. */
#include <cstdint>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <unistd.h>
#include <fcntl.h>
#include "interpret.hh"
#include "sparse_mem.hh"

struct cp_page {
  uint32_t va;
  uint8_t  data[4096];
} __attribute__((packed));

static const uint64_t CKPT_MAGIC = 0x6d697073f5f5d005UL;   /* "mips" + tag */

/* IP22 DRAM is 128 MB @ 0x08000000; the low 512 KB aliases it. Scan a generous
 * window covering all real memory (avoids faulting in the whole 4 GB map). */
static const uint64_t SCAN_LO = 0x00000000UL;
static const uint64_t SCAN_HI = 0x20000000UL;   /* 512 MB */

struct cp_tlb {
  uint64_t entry_hi;
  uint64_t entry_lo0;
  uint64_t entry_lo1;
  uint32_t page_mask;
  uint32_t _pad;
} __attribute__((packed));

struct cp_header {
  uint64_t magic;
  int64_t  pc;
  int64_t  gpr[32];
  int64_t  hi;
  int64_t  lo;
  uint32_t cpr0[32];     /* CP0 (Status = cpr0[12]) */
  uint64_t cpr0_64[32];  /* 64-bit CP0 shadow (EntryHi/EntryLo/XContext) */
  uint64_t cpr1[32];     /* FPRs (FR=1, 64-bit each) */
  uint32_t fcr1[5];      /* FP control regs (incl FCSR) */
  cp_tlb   tlb[48];      /* the full 48-entry TLB */
  uint64_t icnt;
  uint32_t num_pages;
} __attribute__((packed));

static bool page_nonzero(const uint8_t *p) {
  const uint64_t *q = reinterpret_cast<const uint64_t*>(p);
  for(int i = 0; i < 512; i++) if(q[i]) return true;
  return false;
}

void dumpState(const state_t &s, const std::string &filename) {
  cp_header h;
  memset(&h, 0, sizeof(h));
  h.magic = CKPT_MAGIC;
  h.pc = s.pc;
  memcpy(h.gpr, s.gpr, sizeof(h.gpr));
  h.hi = s.hi;
  h.lo = s.lo;
  memcpy(h.cpr0, s.cpr0, sizeof(h.cpr0));
  memcpy(h.cpr0_64, s.cpr0_64, sizeof(h.cpr0_64));
  memcpy(h.cpr1, s.cpr1, sizeof(h.cpr1));
  memcpy(h.fcr1, s.fcr1, sizeof(h.fcr1));
  for(int i = 0; i < 48; i++) {
    h.tlb[i].entry_hi  = s.tlb[i].entry_hi;
    h.tlb[i].entry_lo0 = s.tlb[i].entry_lo0;
    h.tlb[i].entry_lo1 = s.tlb[i].entry_lo1;
    h.tlb[i].page_mask = s.tlb[i].page_mask;
    h.tlb[i]._pad = 0;
  }
  h.icnt = s.icnt;

  uint8_t *base = s.mem.mem;
  uint32_t n = 0;
  for(uint64_t a = SCAN_LO; a < SCAN_HI; a += 4096)
    if(page_nonzero(base + a)) n++;
  h.num_pages = n;

  int fd = ::open(filename.c_str(), O_RDWR|O_CREAT|O_TRUNC, 0600);
  assert(fd != -1);
  ssize_t wb = write(fd, &h, sizeof(h));
  assert(wb == (ssize_t)sizeof(h));
  for(uint64_t a = SCAN_LO; a < SCAN_HI; a += 4096) {
    if(!page_nonzero(base + a)) continue;
    cp_page p;
    p.va = (uint32_t)a;
    memcpy(p.data, base + a, 4096);
    wb = write(fd, &p, sizeof(p));
    assert(wb == (ssize_t)sizeof(p));
  }
  close(fd);
  fprintf(stderr, "[ckpt] wrote %s: pc=%08x, %u pages, icnt=%lu\n",
          filename.c_str(), (uint32_t)s.pc, n, (unsigned long)s.icnt);
}

void loadState(state_t &s, const std::string &filename) {
  int fd = ::open(filename.c_str(), O_RDONLY, 0600);
  assert(fd != -1);
  cp_header h;
  ssize_t rb = read(fd, &h, sizeof(h));
  assert(rb == (ssize_t)sizeof(h));
  assert(h.magic == CKPT_MAGIC);
  s.pc = h.pc;
  memcpy(s.gpr, h.gpr, sizeof(s.gpr));
  s.hi = h.hi;
  s.lo = h.lo;
  memcpy(s.cpr0, h.cpr0, sizeof(s.cpr0));
  memcpy(s.cpr0_64, h.cpr0_64, sizeof(s.cpr0_64));
  memcpy(s.cpr1, h.cpr1, sizeof(s.cpr1));
  memcpy(s.fcr1, h.fcr1, sizeof(s.fcr1));
  for(int i = 0; i < 48; i++) {
    s.tlb[i].entry_hi  = h.tlb[i].entry_hi;
    s.tlb[i].entry_lo0 = h.tlb[i].entry_lo0;
    s.tlb[i].entry_lo1 = h.tlb[i].entry_lo1;
    s.tlb[i].page_mask = h.tlb[i].page_mask;
  }
  s.icnt = h.icnt;
  uint8_t *base = s.mem.mem;
  for(uint32_t i = 0; i < h.num_pages; i++) {
    cp_page p;
    rb = read(fd, &p, sizeof(p));
    assert(rb == (ssize_t)sizeof(p));
    memcpy(base + p.va, p.data, 4096);
  }
  close(fd);
}
