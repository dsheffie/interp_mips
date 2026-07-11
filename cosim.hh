#ifndef __COSIM_HH__
#define __COSIM_HH__
#include <cstdint>

/* Live lockstep co-simulation between interp_mips (server / golden reference) and
 * this JIT sim (client / DUT) over a SysV shared-memory segment (shmem.cc). After
 * every top-level instruction both processes barrier and the server compares
 * architectural state (pc + gpr[32]); the FIRST divergence is printed with both
 * sides' values and the run stops. Enable with --cosim server|client; run BOTH
 * with DETTIME=1 so the RTC clock is identical. */
void cosim_init(bool server);
bool cosim_active();
/* Call once per top-level instruction, AFTER it has executed. Compares pc+gpr
 * every call; also compares mem_hash whenever it is not COSIM_NO_HASH (the caller
 * passes a real hash only every N instructions -- a physical-RAM-region hash --
 * to catch device/DMA memory divergences the register compare can't see).
 * Returns true when a divergence has been detected (the caller should stop). */
static const uint64_t COSIM_NO_HASH = ~0ULL;
/* cp0dbg = {Cause, SR, Count, EPC}: not compared (Count legitimately skews on
 * micro-differences), but printed for BOTH sides at a divergence so interrupt-
 * timing vs synchronous-fault mismatches are immediately visible. */
bool cosim_step(uint64_t icnt, int64_t pc, const int64_t *gpr, uint64_t mem_hash,
                const uint32_t cp0dbg[4]);

#endif
