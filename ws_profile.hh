#ifndef __WS_PROFILE_HH__
#define __WS_PROFILE_HH__
/* Data-cache working-set profiler for the ISS. Hooked in cm_load/cm_store (every
 * cached CPU DATA access, physical addr). A set of N-way LRU caches at swept
 * sizes -> miss rate vs size = working-set curve. henny L1D = 16KB direct-mapped
 * 16B lines, so we include that exact point plus a 4-way sweep. Reset/stop via
 * markers in va_translate (VA 0x30000000 reset, 0x30001000 stop). Env WSPROFILE. */
#include <cstdint>
#include <cstdlib>
#include <vector>
#include <cstdio>
#include <string>

struct waycache {
  uint32_t nsets, ways, lg_line;
  std::vector<uint32_t> tag;   /* nsets*ways */
  std::vector<uint8_t>  cnt;   /* valid ways per set (MRU-ordered at [set*ways+0]) */
  uint64_t acc = 0, miss = 0;
  const char *label;
  waycache(uint32_t size_bytes, uint32_t ways_, uint32_t line, const char *lbl)
    : ways(ways_), label(lbl) {
    lg_line = 0; while((1u<<lg_line) < line) lg_line++;
    nsets = size_bytes / (line * ways);
    tag.assign((size_t)nsets*ways, 0);
    cnt.assign(nsets, 0);
  }
  inline void access(uint32_t pa) {
    acc++;
    uint32_t line = pa >> lg_line;
    uint32_t set  = line % nsets;      /* nsets is a power of two for our sizes */
    uint32_t t    = line / nsets;
    uint32_t base = set * ways;
    uint32_t n = cnt[set];
    for(uint32_t i = 0; i < n; i++) {
      if(tag[base+i] == t) {                       /* hit: move to MRU (front) */
        for(uint32_t j = i; j > 0; j--) tag[base+j] = tag[base+j-1];
        tag[base] = t; return;
      }
    }
    miss++;                                        /* miss: insert at MRU, evict LRU */
    uint32_t keep = (n < ways) ? n : (ways - 1);
    for(uint32_t j = keep; j > 0; j--) tag[base+j] = tag[base+j-1];
    tag[base] = t;
    if(n < ways) cnt[set] = n + 1;
  }
  void reset(){ acc = miss = 0; std::fill(cnt.begin(), cnt.end(), 0); }
};

struct ws_profile {
  std::vector<waycache> c;
  uint64_t icnt0 = 0;   /* instruction count at the last reset (for MPKI) */
  ws_profile() {
    /* henny exact: 16KB direct-mapped, 16B line; r9999 L2: 128KB direct-mapped */
    c.emplace_back(16u*1024, 1, 16, "henny_16K_dm");
    c.emplace_back(128u*1024, 1, 16, "L2_128K_dm");
    /* WSWAYS-way sweep (default 4), 16B line.  WSWIDE=1: 64B..256MB (the
     * interp_rv64-style working-set plot); default 2KB..1MB. */
    static const char *ew = getenv("WSWAYS");
    static const uint32_t ways = ew ? (uint32_t)strtoul(ew, 0, 0) : 4;
    static const bool wide = getenv("WSWIDE") != nullptr;
    const uint64_t lo = wide ? 64ull : 2048ull, hi = wide ? (256ull << 20) : (1ull << 20);
    static char buf[64][24]; static int bi = 0;
    for(uint64_t sz = lo; sz <= hi; sz <<= 1) {
      if(sz < (uint64_t)16 * ways) {
        continue;
      }
      snprintf(buf[bi], 24, "%lluB_%uw", (unsigned long long)sz, ways);
      c.emplace_back((uint32_t)sz, ways, 16, buf[bi]); bi++;
    }
  }
  inline void access(uint32_t pa){ for(auto &w : c) w.access(pa); }
  void reset(uint64_t icnt = 0){ for(auto &w : c) w.reset(); icnt0 = icnt; }
  void dump(const char *tag, uint64_t icnt = 0){
    const double kinsn = (icnt > icnt0) ? (double)(icnt - icnt0) / 1000.0 : 0.0;
    fprintf(stderr, "### WSPROF %s (data accesses=%llu, insns=%llu)\n", tag,
            (unsigned long long)(c.empty()?0:c[0].acc), (unsigned long long)(icnt - icnt0));
    fprintf(stderr, "  %-16s %12s %8s %10s\n", "config", "misses", "miss%%", "mpki");
    for(auto &w : c)
      fprintf(stderr, "  %-16s %12llu %7.3f%% %10.3f\n", w.label,
              (unsigned long long)w.miss, w.acc? 100.0*w.miss/w.acc : 0.0,
              kinsn > 0 ? (double)w.miss / kinsn : 0.0);
    fflush(stderr);
  }
};

extern ws_profile *g_wsprof;
#endif
