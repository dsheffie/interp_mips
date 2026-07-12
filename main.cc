#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <iostream>
#include <string>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <boost/program_options.hpp>

#include "interpret.hh"
#include "loadelf.hh"
#include "sparse_mem.hh"
#include "pseudo_bios.hh"
#include "sgi_mc.hh"
#include "sgi_hpc.hh"
#include "sgi_scc.hh"
#include "sgi_scsi.hh"
#include "gdbstub.hh"
#include "globals.hh"
#include "inst_record.hh"
#include "cosim.hh"
#include <csignal>

namespace po = boost::program_options;

/* debug watchpoint globals (declared extern in sparse_mem.hh) */
uint32_t g_watch_pa = 0, g_watch_ldpc = 0;
uint64_t g_watch_lo = 0, g_watch_hi = ~0ULL;
uint64_t g_cur_pc = 0, g_cur_icnt = 0;
uint64_t g_htrace = 0;

/* Async control via signals (checked once per instruction in the run loop):
 *   SIGUSR1 -> dump the current IRIX process (comm/pid/pc)
 *   SIGUSR2 -> flush the SCSI COW overlay to its --disk-delta sidecar  */
static volatile sig_atomic_t g_sig_dumpproc   = 0;
static volatile sig_atomic_t g_sig_flushdelta = 0;
static void on_sigusr1(int) { g_sig_dumpproc   = 1; }
static void on_sigusr2(int) { g_sig_flushdelta = 1; }
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
};
static state_t *s = nullptr;

int main(int argc, char *argv[]) {
  std::string filename, arcs, retire_name, start_pc, disk, prom, disk_delta, ckpt_at, ckpt_out, restore_name;
  std::string cosim;   /* "server"|"client": lockstep co-sim vs the JIT sim */
  uint64_t maxinsns = ~(0UL);
  uint64_t ckpt_icnt = 0;
  int gdb_port = 0;
  try {
    po::options_description desc("options");
    desc.add_options()
      ("file,f",    po::value<std::string>(&filename), "ELF kernel image")
      ("arcs",      po::value<std::string>(&arcs),     "ARCS firmware blob (loaded at PA 0x1000)")
      ("retiretrace", po::value<std::string>(&retire_name), "emit boost retire_trace for rv64analyzer")
      ("maxicnt,m", po::value<uint64_t>(&maxinsns)->default_value(~(0UL)), "max instructions")
      ("start-pc", po::value<std::string>(&start_pc)->default_value(""), "fake-BIOS: start PC e.g. 0xa0003000 (skips pseudo_bios + arcs patch; firmware does the handoff)")
      ("disk",     po::value<std::string>(&disk),     "raw SCSI disk image for HD0 (e.g. irix65.img)")
      ("disk-delta", po::value<std::string>(&disk_delta), "persistent COW sidecar for --disk: writes survive across runs (image stays read-only); flushed on exit + SIGUSR2")
      ("prom",     po::value<std::string>(&prom),     "flat PROM firmware blob loaded at phys 0x1fc00000 (e.g. henry_arcs.bin); use with --start-pc 0xbfc00000")
      ("checkpoint-at",  po::value<std::string>(&ckpt_at)->default_value(""),  "dump a full-state checkpoint when this PC (hex) first retires, then exit")
      ("checkpoint-icnt", po::value<uint64_t>(&ckpt_icnt)->default_value(0),    "dump a full-state checkpoint when icnt first reaches this value, then exit")
      ("checkpoint-out", po::value<std::string>(&ckpt_out)->default_value("checkpoint.bin"), "checkpoint output file for --checkpoint-at")
      ("restore", po::value<std::string>(&restore_name)->default_value(""), "restore a full-state checkpoint (loadState) and continue from it")
      ("gdb", po::value<int>(&gdb_port)->default_value(0), "listen for gdb on this TCP port (RSP stub); boots full-speed until a client attaches")
      ("cosim", po::value<std::string>(&cosim), "lockstep co-sim: 'server' (golden) or 'client'; run both sims with DETTIME=1");
    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);
  } catch(...) { std::cerr << "bad args\n"; return 1; }

  //if(filename.empty()) { std::cerr << "need --file <kernel>\n"; return 1; }

  sparse_mem *sm = new sparse_mem();
  /* CACHE_MODEL: model the R4x00/Indy write-back cache (32B lines) IRIX is built
   * for, so cache-coherence bugs the perfectly-coherent ISS hides become visible.
   * Off by default -> plain ISS behavior. */
  if(getenv("CACHE_MODEL")) {
    g_cmodel = new cache_model(*sm);
    g_stale_detect = getenv("STALE_DETECT") != nullptr;   /* flag stale cached reads */
    fprintf(stderr, "[cache_model] ENABLED: 32B write-back L1 D-cache (%u sets)%s\n",
            cache_model::L1_SETS, g_stale_detect ? " + STALE_DETECT" : "");
  }
  s = new state_t(*sm);
  s->maxicnt = maxinsns;
  s->mc  = new sgi_mc(s);
  s->hpc = new sgi_hpc(s);
  s->scc = new sgi_scc(s);
  if(!disk.empty()) s->scsi = new sgi_scsi(s, disk, disk_delta);
  sm->st = s;
  sm->route_devices = true;

  initState(s);                    /* CP0 reset state: PRId, Config, SR, Random */
  if(not(filename.empty())) {
    load_elf(filename.c_str(), s);   /* sets s->pc to the entry */
  }

  /* IRIX /unix entry ABI (ARCS): a0=argc, a1=argv, a2=envp. The pseudo-BIOS
   * synthesizes the real argv/envp handoff sash gives /unix (the kernel's
   * getargs/_envirn read these; a1=a2=0 made getargs derail -- MAME Q5). */
  /* fake-BIOS (--start-pc): the arcs_irix boot stub does the argv/envp handoff
   * itself (matching the FPGA), so skip the C++ pseudo-BIOS. */
  if(start_pc.empty()) install_pseudo_bios(s, sm);

  if(!arcs.empty()) {
    int fd = open(arcs.c_str(), O_RDONLY);
    if(fd < 0) { std::cerr << "cannot open arcs " << arcs << "\n"; return 1; }
    struct stat st; fstat(fd, &st);
    void *buf = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    memcpy(sm->mem + 0x1000, buf, st.st_size);
    std::cerr << "loaded ARCS firmware (" << st.st_size << " bytes) at PA 0x1000\n";
    munmap(buf, st.st_size); close(fd);

    /* Patch the ARCS GetEnvironmentVariable stub to return a real "eaddr" string.
     * The r9999-irix arcs_irix blob's stub_getenv returns NULL; with the real TLB
     * enabled, init_sysid then calls etoh(NULL) -> load from VA 0 -> TLB miss and
     * derail (the old 1:1 va2pa masked this by reading PA 0 = zeros, giving a
     * harmless all-zero MAC). Real PROM firmware returns the EEPROM eaddr, so we
     * inject one. Layout: stub_getenv @ blob offset 0xe68; the eaddr ASCII string
     * is placed at blob offset 0xf00 (past the 3840-byte blob, still low RAM).
     * Code (big-endian, as fetched+bswapped): lui v0,0xa000; ori v0,v0,0x1f00;
     * jr ra; nop.  v0 -> 0xa0001f00 (kseg1, uncached).  Only applies to the IRIX
     * blob (stub_getenv @ 0xe68; blob is 0xf00 bytes); the Linux arcs_fw blob is
     * far smaller (752 bytes), so guard on size to avoid clobbering a small blob. */
    if(start_pc.empty() && st.st_size >= 0xe78) {
      uint8_t *base = (uint8_t*)sm->mem + 0x1000;
      const char *eaddr = "08:00:69:12:34:56";
      memcpy(base + 0xf00, eaddr, strlen(eaddr) + 1);
      /* MIPS BE instruction words, written byte-wise as big-endian */
      auto put_be = [&](uint32_t off, uint32_t insn) {
        base[off+0] = (insn >> 24) & 0xff; base[off+1] = (insn >> 16) & 0xff;
        base[off+2] = (insn >>  8) & 0xff; base[off+3] = (insn >>  0) & 0xff;
      };
      put_be(0xe68, 0x3c02a000);   /* lui   v0, 0xa000      */
      put_be(0xe6c, 0x34421f00);   /* ori   v0, v0, 0x1f00  */
      put_be(0xe70, 0x03e00008);   /* jr    ra              */
      put_be(0xe74, 0x00000000);   /* nop                   */
    }
  }

  /* PROM firmware (henry_arcs.bin): a self-contained FSBL linked at the CPU
   * reset vector 0xBFC00000 (phys 0x1fc00000).  It copies its SPB to phys 0x1000,
   * stages argv/envp, reads the kernel entry from the kentry slot @0xBFC00008
   * (default 0x88005960 = the IRIX /unix entry), and jumps to it.  Load the flat
   * blob into the PROM region and boot via --start-pc 0xbfc00000. */
  if(!prom.empty()) {
    int fd = open(prom.c_str(), O_RDONLY);
    if(fd < 0) { std::cerr << "cannot open prom " << prom << "\n"; return 1; }
    struct stat st; fstat(fd, &st);
    void *buf = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    memcpy(sm->mem + 0x1fc00000, buf, st.st_size);
    std::cerr << "loaded PROM firmware (" << st.st_size << " bytes) at phys 0x1fc00000\n";
    munmap(buf, st.st_size); close(fd);

    /* Patch the kentry slot @0xBFC00008 (phys 0x1fc00008) with the loaded
     * kernel's ELF entry, big-endian.  henry_arcs.S hardcodes 0x88005960 (the
     * IRIX /unix entry) as the default and expects "the driver/TB" to patch the
     * slot per kernel; the FSBL jumps there blindly (jr $k0).  Without this,
     * Linux (entry 0x88325eb8) jumps to the IRIX entry and derails.  s->pc still
     * holds the ELF entry here (the --start-pc override runs below). */
    uint32_t kentry = (uint32_t)s->pc;
    uint8_t *kslot = (uint8_t*)sm->mem + 0x1fc00008;
    kslot[0] = (kentry >> 24) & 0xff; kslot[1] = (kentry >> 16) & 0xff;
    kslot[2] = (kentry >>  8) & 0xff; kslot[3] = (kentry >>  0) & 0xff;
    std::cerr << "patched kentry slot @0xbfc00008 = " << std::hex << kentry << std::dec << "\n";
  }

  /* fake-BIOS: start at the firmware boot stub (e.g. arcs_boot @ 0xa0003000);
   * the stub prints the SCC heartbeat, builds argv/envp, and jumps to /unix. */
  if(!start_pc.empty()) {
    s->pc = (int64_t)(int32_t)(uint32_t)strtoul(start_pc.c_str(), nullptr, 0);
    std::cerr << "fake-bios: start pc = " << std::hex << (uint32_t)s->pc << std::dec << "\n";
  }

  if(!restore_name.empty()) {
    loadState(*s, restore_name);
    std::cerr << "restored checkpoint " << restore_name << " -> pc=" << std::hex
              << (uint32_t)s->pc << " icnt=" << std::dec << s->icnt << "\n";
  }

  retire_trace rt;
  if(!retire_name.empty()) {
    globals::retire_log = &rt;
    globals::trace_retirement = true;
  }

  const char *pctrace = getenv("PCTRACE");
  uint64_t pcsample = pctrace ? strtoull(pctrace, nullptr, 0) : 0;
  const char *fw = getenv("FINEWIN");   /* "lo:hi" icnt window for per-insn PC trace */
  uint64_t flo=0, fhi=0;
  if(fw) { sscanf(fw, "%lu:%lu", &flo, &fhi); }
  const char *a1p = getenv("A1PROBE"); /* dump a0/a1/a2 first few times pc hits this addr */
  uint32_t probe_pc = a1p ? (uint32_t)strtoull(a1p, nullptr, 0) : 0;
  const char *a1after = getenv("A1PROBE_AFTER"); /* only start probing past this icnt */
  uint64_t probe_after = a1after ? strtoull(a1after, nullptr, 0) : 0;
  int probe_hits = 0;
  uint32_t ckpt_pc = ckpt_at.empty() ? 0 : (uint32_t)strtoull(ckpt_at.c_str(), nullptr, 0);
  /* PCTRACEOUT=<file>: co-sim trace -- one hex virtual PC per retired
   * instruction (delay slots included; emitted inside execMips), starting when
   * execution first reaches PCTRACE_START (default kernel entry 0x88005960). */
  const char *pcto = getenv("PCTRACEOUT");
  if(pcto) globals::pctrace = fopen(pcto, "w");
  const char *pcs = getenv("PCTRACE_START");
  if(pcs) globals::pctrace_start = (uint32_t)strtoull(pcs, nullptr, 0);
  /* VALTRACE=<file>: per-USER-instruction "pc dst val" co-sim value trace
   * (dst = the GPR changed by the instruction, -1 if none; via before/after diff). */
  const char *vto = getenv("VALTRACE");
  FILE *valt = vto ? fopen(vto, "w") : nullptr;
  const bool valt_all = getenv("VALTRACE_ALL") != nullptr;  /* trace all insns (kernel co-sim) */
  uint64_t gprsnap[32];
  
  signal(SIGUSR1, on_sigusr1);   /* kill -USR1 <pid> -> dump current IRIX process */
  signal(SIGUSR2, on_sigusr2);   /* kill -USR2 <pid> -> flush --disk-delta sidecar */
  if(gdb_port) s->gdb = new gdb_stub(gdb_port);
  if(cosim == "server")      cosim_init(true);
  else if(cosim == "client") cosim_init(false);
  const char *mnenv = getenv("COSIM_MEMN");
  uint64_t cosim_memn = mnenv ? strtoull(mnenv, nullptr, 0) : 4000000;  /* co-sim RAM-hash interval */
  const char *msenv = getenv("COSIM_MEMSTART");
  uint64_t cosim_memstart = msenv ? strtoull(msenv, nullptr, 0) : 0;
  { const char *e;   /* memory-op watchpoints (see sparse_mem.hh) */
    if((e = getenv("WATCHPA")))   g_watch_pa   = (uint32_t)strtoull(e, 0, 0);
    if((e = getenv("WATCHLDPC"))) g_watch_ldpc = (uint32_t)strtoull(e, 0, 0);
    if((e = getenv("WATCH_LO")))  g_watch_lo   = strtoull(e, 0, 0);
    if((e = getenv("WATCH_HI")))  g_watch_hi   = strtoull(e, 0, 0);
  }

  double t0 = timestamp();
  while(s->brk == 0 && s->icnt < s->maxicnt) {
    if(g_sig_dumpproc)   { g_sig_dumpproc = 0;   dump_current_process(s); }
    if(g_sig_flushdelta) { g_sig_flushdelta = 0; if(s->scsi) s->scsi->flush_delta(); }
    if(pcsample && (s->icnt % pcsample) == 0)
      fprintf(stderr, "[pc] icnt=%lu pc=%08x ra=%08x sp=%08x\n",
              (unsigned long)s->icnt, (uint32_t)s->pc, (uint32_t)s->gpr[31], (uint32_t)s->gpr[29]);
    if(fw && s->icnt>=flo && s->icnt<fhi)
      fprintf(stderr, "[fine] icnt=%lu pc=%08x\n", (unsigned long)s->icnt, (uint32_t)s->pc);
    if(probe_pc && s->icnt >= probe_after && (uint32_t)s->pc == probe_pc) {
      fprintf(stderr, "[probe] icnt=%lu pc=%08x a0=%016lx a1=%016lx a2=%016lx a3=%016lx a4=%016lx a5=%016lx\n",
              (unsigned long)s->icnt, (uint32_t)s->pc,
              (long)s->gpr[4], (long)s->gpr[5], (long)s->gpr[6],
              (long)s->gpr[7], (long)s->gpr[8], (long)s->gpr[9]);
      if(++probe_hits >= 64) break;
    }
    if(ckpt_pc && (uint32_t)s->pc == ckpt_pc) {
      fprintf(stderr, "[ckpt] reached pc=%08x at icnt=%lu -> dumping %s\n",
              ckpt_pc, (unsigned long)s->icnt, ckpt_out.c_str());
      dumpState(*s, ckpt_out);
      break;
    }
    if(ckpt_icnt && s->icnt >= ckpt_icnt) {
      fprintf(stderr, "[ckpt] reached icnt=%lu (pc=%08x) -> dumping %s\n",
              (unsigned long)s->icnt, (uint32_t)s->pc, ckpt_out.c_str());
      dumpState(*s, ckpt_out);
      break;
    }
    if(valt) { for(int gi=0; gi<32; gi++) gprsnap[gi]=(uint64_t)s->gpr[gi]; }
    maybe_take_interrupt(s);   /* CP0 Count/Compare timer + Int delivery (per step) */
    if(s->gdb) s->gdb->step_hook(s);   /* gdb RSP: breakpoints / attach / step */
    uint64_t valt_pc = (uint64_t)s->pc;
    g_cur_pc = (uint64_t)s->pc; g_cur_icnt = s->icnt;   /* context for memory watchpoints */
    if(g_watch_ldpc && (uint32_t)s->pc == g_watch_ldpc &&
       s->icnt >= g_watch_lo && s->icnt < g_watch_hi)
      fprintf(stderr, "[ldbase] icnt=%lu pc=%08x r15=%016llx va=%016llx\n",
              (unsigned long)s->icnt, (uint32_t)s->pc,
              (unsigned long long)s->gpr[15], (unsigned long long)(s->gpr[15] + 60));
    execMips(s);
    if(g_htrace) {   /* kernel TLB-Mod handler trace: executed pc + kernel scratch + arg regs */
      fprintf(stderr, "[HTRACE] pc=%08x k0=%016llx k1=%016llx a0=%016llx a1=%016llx a2=%016llx a3=%016llx cause=%02x epc=%08x sr=%08x\n",
              (uint32_t)valt_pc,
              (unsigned long long)s->gpr[26], (unsigned long long)s->gpr[27],
              (unsigned long long)s->gpr[4], (unsigned long long)s->gpr[5],
              (unsigned long long)s->gpr[6], (unsigned long long)s->gpr[7],
              (s->cpr0[CPR0_CAUSE]>>2)&0x1f, s->cpr0[CPR0_EPC], s->cpr0[CPR0_SR]);
      g_htrace--;
    }
    if(valt && (valt_all || (valt_pc>=0x120000000ULL && valt_pc<0x120640000ULL))) {
      int vd=-1; for(int gi=1; gi<32; gi++) if((uint64_t)s->gpr[gi]!=gprsnap[gi]) { vd=gi; break; }
      fprintf(valt, "%llx %d %llx\n", (unsigned long long)valt_pc, vd,
              (unsigned long long)(vd<0?0:(uint64_t)s->gpr[vd]));
    }
    uint64_t mh = COSIM_NO_HASH;
    if(cosim_active() && s->icnt >= cosim_memstart && (s->icnt % cosim_memn) == 0) {
      const uint64_t *w = reinterpret_cast<const uint64_t*>(s->mem.mem + 0x08000000u);
      uint64_t h = 1469598103934665603ULL;       /* FNV-1a over low DRAM [0x08000000,0x18000000) (must match DUT) */
      for(size_t i = 0; i < (0x10000000u/8); i++) h = (h ^ w[i]) * 1099511628211ULL;
      mh = h;
    }
    uint32_t cp0dbg[4] = { s->cpr0[CPR0_CAUSE], s->cpr0[CPR0_SR],
                           s->cpr0[CPR0_COUNT], s->cpr0[CPR0_EPC] };
    if(cosim_step(s->icnt, (int64_t)s->pc, (const int64_t*)s->gpr, mh, cp0dbg)) break;   /* lockstep co-sim */
  }
  if(cosim_active()) {   /* COSIM_RAMDUMP=<file>: dump low DRAM at the stop point to diff */
    const char *rd = getenv("COSIM_RAMDUMP");
    if(rd) { FILE *f = fopen(rd, "wb"); if(f) { fwrite(s->mem.mem + 0x08000000u, 1, 0x10000000u, f); fclose(f);
      fprintf(stderr, "[cosim] dumped low DRAM to %s at icnt=%lu\n", rd, (unsigned long)s->icnt); } }
  }
  t0 = timestamp() - t0;

  if(s->scsi) s->scsi->flush_delta();   /* persist COW overlay on clean exit (no-op without --disk-delta) */

  if(globals::pctrace) fclose(globals::pctrace);
  std::cout << "\n" << s->icnt << " instructions executed, brk=" << (int)s->brk << "\n";
  std::cout << (static_cast<double>(s->icnt) / t0)*1e-6 << " minsns/sec\n";
  if(!retire_name.empty() && !rt.empty()) {
    std::ofstream ofs(retire_name, std::ios::binary);
    boost::archive::binary_oarchive oa(ofs);
    oa << rt;
    std::cout << "wrote " << rt.get_records().size()
              << " retire_trace records to " << retire_name << "\n";
  }

  delete s->mc; delete s->hpc; delete s->scc; delete s->scsi; delete s; delete sm;
  return 0;
}
