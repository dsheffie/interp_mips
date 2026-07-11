#include "cosim.hh"
#include "shmem.hh"
#include <cstdio>
#include <cstdlib>

/* Shared state the client (DUT) posts each instruction for the server (golden)
 * to compare. Lives in the shmem data region. */
namespace {
  struct cosim_shared {
    volatile uint64_t icnt;
    volatile uint64_t pc;
    volatile int64_t  gpr[32];
    volatile uint64_t mem_hash;
    volatile uint32_t cp0dbg[4];   /* Cause, SR, Count, EPC -- printed, not compared */
    volatile int      diverged;
  };
  shmem *g_shm = nullptr;
  bool g_server = false;
  cosim_shared *g_cs = nullptr;
}

void cosim_init(bool server) {
  g_server = server;
  if(server) {
    shmServer *s = new shmServer(SHM_BYTES, SHM_KEY);
    g_shm = s;
    fprintf(stderr, "[cosim] server (golden): waiting for client...\n");
    s->waitForConnection();
    fprintf(stderr, "[cosim] client connected.\n");
  } else {
    g_shm = new shmClient(SHM_BYTES, SHM_KEY);
    fprintf(stderr, "[cosim] client (DUT): attached to server.\n");
  }
  g_cs = reinterpret_cast<cosim_shared*>(g_shm->dataPtr);
}

bool cosim_active() { return g_shm != nullptr; }

bool cosim_step(uint64_t icnt, int64_t pc, const int64_t *gpr, uint64_t mem_hash,
                const uint32_t cp0dbg[4]) {
  if(!g_shm) return false;
  if(g_server) {
    g_shm->fastSemaDown(0);             /* wait for the client to post insn-i state (spin) */
    bool bad = (g_cs->pc != (uint64_t)pc);
    int badreg = -1;
    for(int i = 1; i < 32; i++) if(g_cs->gpr[i] != gpr[i]) { bad = true; badreg = i; break; }
    bool membad = (mem_hash != COSIM_NO_HASH) && (g_cs->mem_hash != mem_hash);
    if(bad || membad) {
      fprintf(stderr, "\n[cosim] *** DIVERGENCE *** golden icnt=%llu  dut icnt=%llu%s\n",
              (unsigned long long)icnt, (unsigned long long)g_cs->icnt,
              membad && !bad ? "  (MEMORY-only: register state still matches)" : "");
      fprintf(stderr, "  pc:      golden=%016llx  dut=%016llx%s\n",
              (unsigned long long)pc, (unsigned long long)g_cs->pc,
              (g_cs->pc != (uint64_t)pc) ? "   <<<" : "");
      for(int i = 1; i < 32; i++)
        if(g_cs->gpr[i] != gpr[i])
          fprintf(stderr, "  gpr[%2d]: golden=%016llx  dut=%016llx   <<<\n", i,
                  (unsigned long long)gpr[i], (unsigned long long)g_cs->gpr[i]);
      if(membad)
        fprintf(stderr, "  mem_hash: golden=%016llx  dut=%016llx   <<< (RAM diverged within the last hash interval)\n",
                (unsigned long long)mem_hash, (unsigned long long)g_cs->mem_hash);
      fprintf(stderr, "  cp0:     golden Cause=%08x SR=%08x Count=%08x EPC=%08x\n",
              cp0dbg[0], cp0dbg[1], cp0dbg[2], cp0dbg[3]);
      fprintf(stderr, "           dut    Cause=%08x SR=%08x Count=%08x EPC=%08x\n",
              g_cs->cp0dbg[0], g_cs->cp0dbg[1], g_cs->cp0dbg[2], g_cs->cp0dbg[3]);
      g_cs->diverged = 1;
    }
    g_shm->fastSemaUp(1);               /* release the client */
    return bad || membad;
  } else {
    g_cs->icnt = icnt;
    g_cs->pc = (uint64_t)pc;
    for(int i = 0; i < 32; i++) g_cs->gpr[i] = gpr[i];
    g_cs->mem_hash = mem_hash;
    for(int i = 0; i < 4; i++) g_cs->cp0dbg[i] = cp0dbg[i];
    g_shm->fastSemaUp(0);               /* post insn-i state */
    g_shm->fastSemaDown(1);             /* wait for the server's compare */
    return g_cs->diverged != 0;
  }
}
