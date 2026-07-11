#include <cstdio>
#include "cache_model.hh"
#include "sparse_mem.hh"

/* non-null once main() sees CACHE_MODEL in the environment */
cache_model *g_cmodel = nullptr;
bool g_stale_detect = false;

void cache_model::report_stale(uint32_t pa, uint8_t cached, uint8_t dram) {
  if(n_stale < 40) {
    fprintf(stderr, "[STALE-READ] icnt=%llu pc=%08x pa=%08x cached=%02x dram=%02x"
            " (clean cache line filled before DMA wrote DRAM -> missing invalidate)\n",
            (unsigned long long)g_cur_icnt, (uint32_t)g_cur_pc, pa, cached, dram);
  }
  n_stale++;
}

cache_model::cache_model(sparse_mem &m) : dram(m), l1(L1_SETS) {}

/* raw DRAM (backing) byte access -- same path device DMA uses (get_raw_ptr),
 * so the model and the DMA engine agree on physical bytes. */
uint8_t cache_model::dram_rd(uint32_t pa)            { return *dram.get_raw_ptr(pa); }
void    cache_model::dram_wr(uint32_t pa, uint8_t v) { *dram.get_raw_ptr(pa) = v; }

void cm_load (cache_model *cm, uint32_t pa, void *dst, int n)       { cm->cload(pa, dst, n); }
void cm_store(cache_model *cm, uint32_t pa, const void *src, int n) { cm->cstore(pa, src, n); }
