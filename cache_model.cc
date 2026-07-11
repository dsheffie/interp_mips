#include "cache_model.hh"
#include "sparse_mem.hh"

/* non-null once main() sees CACHE_MODEL in the environment */
cache_model *g_cmodel = nullptr;

cache_model::cache_model(sparse_mem &m) : dram(m), l1(L1_SETS) {}

/* raw DRAM (backing) byte access -- same path device DMA uses (get_raw_ptr),
 * so the model and the DMA engine agree on physical bytes. */
uint8_t cache_model::dram_rd(uint32_t pa)            { return *dram.get_raw_ptr(pa); }
void    cache_model::dram_wr(uint32_t pa, uint8_t v) { *dram.get_raw_ptr(pa) = v; }

void cm_load (cache_model *cm, uint32_t pa, void *dst, int n)       { cm->cload(pa, dst, n); }
void cm_store(cache_model *cm, uint32_t pa, const void *src, int n) { cm->cstore(pa, src, n); }
