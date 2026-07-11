#ifndef __CACHE_MODEL_HH__
#define __CACHE_MODEL_HH__
/*
 * cache_model -- a software model of the R4x00/SGI-Indy cache the way IRIX is
 * COMPILED for it (32-byte primary lines; 128-byte secondary lines to follow).
 * The plain ISS treats memory as perfectly coherent, so it can never see the
 * cache-management bugs IRIX trips (it uses the ISA's CACHE ops + cached DMA
 * buffers to the fullest, where Linux stays portable with uncached pages).
 *
 * Model = write-back D-cache over sparse_mem (DRAM).  A CPU cached store lands
 * in the cache line, NOT DRAM; DRAM (what device DMA sees via get_raw_ptr) only
 * updates on a CACHE writeback (Hit_WB / Index_WB) or a natural eviction.  So:
 *   - CPU reads its own store (cache)                     -> coherent w/ itself
 *   - a DMA read of a dirty-cached PA reads STALE DRAM     -> needs a WB first
 *   - a DMA write to a cached PA is shadowed by the line   -> needs an INVAL first
 * i.e. the model enforces exactly the flush/inval contract IRIX must honor.
 *
 * Phase 1 (here): 32B primary D-cache, direct-mapped, 16KB (the R4600 geometry
 * the Config reg already advertises), spec-correct CACHE op matrix.  Phase 2:
 * add the 128B secondary level (L1 flush -> L2 -> DRAM) + I-cache for code
 * coherence.  Enabled by env CACHE_MODEL (default off -> plain ISS unchanged).
 * cf project_coherent_dma_plan / the "IRIX uses every MIPS feature" analysis.
 */
#include <cstdint>
#include <cstring>
#include <vector>

class sparse_mem;

class cache_model {
public:
  /* geometry the OS was built for */
  static const uint32_t L1_LINE = 32;                 /* primary line */
  static const uint32_t L1_SIZE = 16u * 1024u;        /* 16 KB primary D$ */
  static const uint32_t L1_SETS = L1_SIZE / L1_LINE;  /* 512 (direct-mapped) */
  static const uint32_t L1_OFF  = L1_LINE - 1;

  struct cline {
    bool     valid = false;
    bool     dirty = false;
    uint32_t tag   = 0;
    uint8_t  data[L1_LINE];
  };

  sparse_mem &dram;
  std::vector<cline> l1;
  /* stats -- observability for the coherence hunt */
  uint64_t n_fill = 0, n_evict = 0, n_wb = 0, n_inval = 0, n_cstore = 0, n_cload = 0;

  explicit cache_model(sparse_mem &m);

  static inline uint32_t set_of(uint32_t pa) { return (pa / L1_LINE) % L1_SETS; }
  static inline uint32_t tag_of(uint32_t pa) { return (pa / L1_LINE) / L1_SETS; }
  static inline uint32_t line_base(uint32_t tag, uint32_t set) {
    return (tag * L1_SETS + set) * L1_LINE;
  }

  /* raw DRAM byte access (bypasses the model) -- defined in the .cc where
   * sparse_mem is complete. */
  uint8_t  dram_rd(uint32_t pa);
  void     dram_wr(uint32_t pa, uint8_t v);

  /* bring pa's line resident in L1 (evicting+writing-back a dirty conflict) */
  cline &fill(uint32_t pa) {
    uint32_t set = set_of(pa), tag = tag_of(pa);
    cline &l = l1[set];
    if(l.valid && l.tag == tag) {
      return l;
    }
    if(l.valid && l.dirty) {                       /* evict dirty -> DRAM */
      uint32_t base = line_base(l.tag, set);
      for(uint32_t i = 0; i < L1_LINE; i++) { dram_wr(base + i, l.data[i]); }
      n_evict++;
    }
    uint32_t base = pa & ~L1_OFF;                  /* fill from DRAM */
    for(uint32_t i = 0; i < L1_LINE; i++) { l.data[i] = dram_rd(base + i); }
    l.valid = true; l.dirty = false; l.tag = tag; n_fill++;
    return l;
  }

  /* CPU cached byte access (get/set route here through cm_load/cm_store) */
  void cload(uint32_t pa, void *dst, int n) {
    uint8_t *d = static_cast<uint8_t *>(dst);
    for(int i = 0; i < n; i++) {
      cline &l = fill(pa + i);
      d[i] = l.data[(pa + i) & L1_OFF];
    }
    n_cload++;
  }
  void cstore(uint32_t pa, const void *src, int n) {
    const uint8_t *s = static_cast<const uint8_t *>(src);
    for(int i = 0; i < n; i++) {
      cline &l = fill(pa + i);
      l.data[(pa + i) & L1_OFF] = s[i];
      l.dirty = true;
    }
    n_cstore++;
  }

  /* --- CACHE op matrix, spec-correct at 32B granularity --- */
  void wb(cline &l, uint32_t set) {                /* push dirty line -> DRAM */
    if(l.valid && l.dirty) {
      uint32_t base = line_base(l.tag, set);
      for(uint32_t i = 0; i < L1_LINE; i++) { dram_wr(base + i, l.data[i]); }
      l.dirty = false; n_wb++;
    }
  }
  /* Hit-type: translated PA, act only if the line is resident (tag match) */
  void hit_wb(uint32_t pa)       { uint32_t s = set_of(pa); cline &l = l1[s]; if(l.valid && l.tag == tag_of(pa)) { wb(l, s); } }
  void hit_inval(uint32_t pa)    { uint32_t s = set_of(pa); cline &l = l1[s]; if(l.valid && l.tag == tag_of(pa)) { l.valid = false; n_inval++; } }
  void hit_wb_inval(uint32_t pa) { uint32_t s = set_of(pa); cline &l = l1[s]; if(l.valid && l.tag == tag_of(pa)) { wb(l, s); l.valid = false; n_inval++; } }
  /* Index-type: the address' index bits select the SET directly (no tag match) */
  void index_inval(uint32_t idx)    { cline &l = l1[set_of(idx)]; if(l.valid) { l.valid = false; n_inval++; } }
  void index_wb_inval(uint32_t idx) { uint32_t s = set_of(idx); wb(l1[s], s); if(l1[s].valid) { l1[s].valid = false; n_inval++; } }
};

/* free-function glue so sparse_mem.hh needs only a forward-decl (no circular
 * include of the template-inline get<>/set<>). Defined in cache_model.cc. */
extern cache_model *g_cmodel;                 /* non-null when CACHE_MODEL is on */
void cm_load (cache_model *cm, uint32_t pa, void *dst, int n);
void cm_store(cache_model *cm, uint32_t pa, const void *src, int n);

#endif
