/* em_main.cc -- Emscripten entry for interp_mips (Jazz/Magnum) in the browser.
 *
 * Boots either the bundled clean-room Linux/Jazz (our ARC shim + GPL kernel, embedded in the .data)
 * or Windows NT 4.0 if the user uploads NTPROM.RAW + a disk image (+ nvram).  The G364 framebuffer
 * renders to the SDL2 canvas and the i8042 takes keyboard/mouse; the run loop is an
 * emscripten_set_main_loop slice so the browser stays responsive.
 *
 * This replaces the native main.cc for the WASM build only (that file keeps Boost program_options,
 * the gdb/cosim sockets, capstone and libunwind, none of which are in the web build). */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <emscripten.h>

#include "interpret.hh"
#include "sparse_mem.hh"
#include "jazz.hh"
#include "jazz_sdl.hh"
#include "loadelf.hh"
#include "pseudo_bios.hh"
#include "cache_model.hh"
#include "globals.hh"
#include "inst_record.hh"

/* globals:: lives in the native main.cc; re-provide it here for the WASM link. */
namespace globals {
  bool enClockFuncts = false;
  uint64_t icountMIPS = 0;
  uint64_t cycle = 0;
  bool trace_retirement = false;
  bool trace_fp = false;
  bool report_syscalls = false;
  retire_trace *retire_log = nullptr;
  FILE *pctrace = nullptr;
  uint32_t pctrace_start = 0x88005960u;
  bool pctrace_on = false;
  bool asidpc_armed = false;
};

/* capstone + the disassembler are excluded; interpret.cc calls these only on debug/dump paths. */
std::string getAsmString(uint32_t inst, uint32_t addr) { return std::string(); }
const std::string &getCondName(uint32_t) { static const std::string e; return e; }

/* file-scope globals that live in the native main.cc (watchpoints / trace context); inert here. */
uint32_t g_watch_pa = 0, g_watch_ldpc = 0;
uint64_t g_watch_lo = 0, g_watch_hi = ~0ULL;
uint64_t g_cur_pc = 0, g_cur_icnt = 0;
uint64_t g_htrace = 0;

/* ---- NT disk: sparse, in-memory, built by JS during upload (see index.html) ----
 * The 4 GiB raw is ~99.5% zeros, so JS keeps only the non-zero 64 KiB blocks in Module.ntDisk
 * (a Map).  These shims serve the base image to jazz_esp synchronously (the data is already
 * resident); writes stay in jazz_esp's C-side overlay, so the user's file is never modified. */
EM_JS(int, em_disk_present, (void), {
  return (Module.ntDisk && Module.ntDisk.blocks) ? 1 : 0;
});
EM_JS(unsigned, em_disk_nblocks, (void), {
  return Module.ntDisk ? (Module.ntDisk.nblocks >>> 0) : 0;
});
EM_JS(void, em_disk_read, (unsigned lba, uint8_t *dst), {
  var d = Module.ntDisk;
  var byteOff = lba * 512;                 /* JS doubles carry this past 2^32 fine */
  var bs = d.blockSize;
  var blk = Math.floor(byteOff / bs);
  var within = byteOff - blk * bs;
  var b = d.blocks.get(blk);
  if (b) { HEAPU8.set(b.subarray(within, within + 512), dst); }
  else   { HEAPU8.fill(0, dst, dst + 512); }   /* unmapped == zeros */
});

static state_t *s = nullptr;
static sparse_mem *sm = nullptr;
static jazz_sdl *disp = nullptr;
static bool g_running = false;

/* Push the live run stats to the page's #stats row (see index.html). */
EM_JS(void, em_stats, (double mips, double nop_ds, double icnt), {
  if(typeof Module.updateStats === 'function') { Module.updateStats(mips, nop_ds, icnt); }
});

/* Load a flat file (embedded or uploaded, in MEMFS) into a heap buffer. */
static uint8_t *slurp(const char *path, size_t *len_out) {
  FILE *f = fopen(path, "rb");
  if(!f) { return nullptr; }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *buf = (uint8_t *)malloc((size_t)n);
  if(fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return nullptr; }
  fclose(f);
  if(len_out) { *len_out = (size_t)n; }
  return buf;
}

/* Load the ARC firmware shim at PA 0x1000 (alias-routed), matching main.cc's --arcs path. */
static bool load_arcs(const char *path) {
  size_t n = 0;
  uint8_t *buf = slurp(path, &n);
  if(!buf) { printf("[em] cannot open arcs %s\n", path); return false; }
  memcpy(sm->get_raw_ptr(0x1000), buf, n);
  printf("[em] loaded ARCS firmware (%zu bytes) at PA 0x1000\n", n);
  free(buf);
  return true;
}

/* One animation-frame slice.  Keep each frame's work bounded so the browser stays live. */
static void em_step() {
  if(!g_running) { return; }
  if(s->brk || s->icnt >= s->maxicnt) {
    if(disp) { disp->pump(); }                 /* final frame */
    g_running = false;
    emscripten_cancel_main_loop();
    printf("\n[em] stopped: %llu instructions, brk=%d\n",
           (unsigned long long)s->icnt, (int)s->brk);
    return;
  }
  const uint64_t slice = 1000000;              /* ~1M insns/frame */
  uint64_t end = s->icnt + slice;
  while(s->icnt < end && !s->brk && s->icnt < s->maxicnt) {
    maybe_take_interrupt(s);
    execMips(s);
  }
  if(disp) { disp->pump(); }                   /* poll input + render the G364 (throttled to ~30 Hz) */

  /* Refresh the page stats ~2x/sec: MIPS over the elapsed window (smoother than
   * per-frame, which is render-jittered), plus live icnt + NOP-delay-slot count. */
  static double stat_t0 = 0;                   /* wall-clock (ms) at last emit */
  static uint64_t stat_icnt0 = 0;              /* s->icnt at last emit */
  double now = emscripten_get_now();
  if(stat_t0 == 0) { stat_t0 = now; stat_icnt0 = s->icnt; }
  else if(now - stat_t0 >= 500.0) {
    double mips = (double)(s->icnt - stat_icnt0) / (now - stat_t0) / 1000.0;  /* insns/ms -> MIPS */
    em_stats(mips, (double)s->nop_delay_slots, (double)s->icnt);
    stat_t0 = now;
    stat_icnt0 = s->icnt;
  }
}

/* Build the Jazz platform and boot.  nt=false -> bundled clean-room Linux; nt=true -> uploaded NT. */
static void boot(bool nt) {
  sm = new sparse_mem();
  s = new state_t(*sm);
  s->maxicnt = ~0ULL;
  sm->st = s;

  const char *nvram = nt ? "/nvram" : "";      /* NT needs its DS1225; Linux uses an anon buffer */
  /* no file-backed disk in wasm: the NT disk is the JS sparse store (jazz_esp uses em_disk_*). */
  s->jz = new jazz(s, nvram, 50000, "", false, "");
  sm->route_jazz = true;

#ifndef EM_HEADLESS
  disp = new jazz_sdl(s->jz, s);                 /* SDL window/canvas needs a browser DOM */
  if(!disp->ok()) { delete disp; disp = nullptr; printf("[em] no SDL canvas; headless\n"); }
#endif

  initState(s);
  if(nt) {
    /* NT: run the vendor PROM from the reset vector (it loads osloader itself). */
    size_t n = 0; uint8_t *rom = slurp("/NTPROM.RAW", &n);
    if(!rom) { printf("[em] NT needs NTPROM.RAW uploaded\n"); return; }
    /* get_raw_ptr applies em_fold, so these land in the compact wasm buffer (0x08800000 /
     * 0x08c00000), not at the raw 0x1fc00000 / 0xfff00000 which is far past the 144 MiB map. */
    memcpy(sm->get_raw_ptr(0x1fc00000ULL), rom, n);
    memcpy(sm->get_raw_ptr(0xfff00000ULL), rom, n);   /* Jazz PROM alias */
    free(rom);
    s->pc = (int64_t)(int32_t)0xbfc00000u;
    printf("[em] booting Windows NT 4.0 from NTPROM.RAW\n");
  }
  else {
    /* Linux: load our ARC shim + the kernel ELF, start at the kernel entry (pseudo_bios sets argv). */
    load_elf("/vmlinux.32", s);                /* sets s->pc = kernel entry */
    install_pseudo_bios(s, sm);                /* a0/a1/a2 = argc/argv/envp */
    if(!load_arcs("/jazz_arcs.bin")) { return; }
    printf("[em] booting clean-room Linux/Jazz\n");
  }

  g_running = true;
  emscripten_set_main_loop(em_step, 0, 0);     /* 0 = requestAnimationFrame cadence */
}

/* Exposed to JS: window.Module._boot_linux() / _boot_nt(). */
extern "C" {
  EMSCRIPTEN_KEEPALIVE void boot_linux() { if(!g_running) { boot(false); } }
  EMSCRIPTEN_KEEPALIVE void boot_nt()    { if(!g_running) { boot(true);  } }
}

int main() {
  printf("interp_mips (Jazz/Magnum) -- WASM build. Call Module._boot_linux() or _boot_nt().\n");
  return 0;                                    /* idle; JS kicks off a boot */
}
