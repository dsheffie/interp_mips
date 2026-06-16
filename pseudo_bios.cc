#include "pseudo_bios.hh"
#include "interpret.hh"
#include "sparse_mem.hh"

#include <cstdint>
#include <cstring>
#include <vector>

namespace {

/* What MAME's sash hands /unix (measured ground truth, MAME_QUESTIONS.md Q5). */
static const char *const kArgv[] = {
  "scsi(0)disk(1)rdisk(0)partition(0)/unix",
  "OSLoadOptions=auto",
  "ConsoleIn=serial(0)",
  "ConsoleOut=serial(0)",
  "SystemPartition=scsi(0)disk(1)rdisk(0)partition(8)",
  "OSLoader=sash",
  "OSLoadPartition=scsi(0)disk(1)rdisk(0)partition(0)",
  "OSLoadFilename=/unix",
};
static const char *const kEnvp[] = {
  "AutoLoad=Yes",
  "TimeZone=PST8PDT",
  "console=d",
  "diskless=0",
  "dbaud=9600",
  "volume=80",
  "sgilogo=y",
  "autopower=y",
  "eaddr=08:01:02:03:04:05",
  "ConsoleOut=serial(0)",
  "ConsoleIn=serial(0)",
  "cpufreq=100",
  "SystemPartition=scsi(0)disk(1)rdisk(0)partition(8)",
  "OSLoadPartition=scsi(0)disk(1)rdisk(0)partition(0)",
  "OSLoadFilename=/unix",
  "OSLoader=sash",
  "kernname=scsi(0)disk(1)rdisk(0)partition(0)/unix",
};

} // namespace

void install_pseudo_bios(state_t *s, sparse_mem *sm) {
  /* Scratch arena near the top of the 16 MiB RAM bank (PA 0x08000000..0x09000000);
   * sash leaves argv/envp here and the kernel reads them in early mlsetup, before
   * memory init zeroes anything. kseg0 pointer = 0x80000000 | PA. */
  uint32_t pa = 0x08fff000;
  uint8_t *m = sm->mem;

  auto kseg0  = [](uint32_t p) -> uint32_t { return 0x80000000u | p; };
  auto sext32 = [](uint32_t v) -> int64_t  { return (int64_t)(int32_t)v; };
  auto put_str = [&](const char *str) -> uint32_t {       /* -> kseg0 ptr */
    uint32_t at = pa;
    size_t n = std::strlen(str) + 1;
    std::memcpy(m + pa, str, n);
    pa += (uint32_t)n;
    return kseg0(at);
  };
  auto put_be32 = [&](uint32_t v) {                       /* big-endian word */
    m[pa+0] = (uint8_t)(v >> 24); m[pa+1] = (uint8_t)(v >> 16);
    m[pa+2] = (uint8_t)(v >> 8);  m[pa+3] = (uint8_t)v;
    pa += 4;
  };

  const int argc = (int)(sizeof(kArgv) / sizeof(kArgv[0]));
  std::vector<uint32_t> argptrs, envptrs;
  for(const char *a : kArgv) argptrs.push_back(put_str(a));
  for(const char *e : kEnvp) envptrs.push_back(put_str(e));

  pa = (pa + 3u) & ~3u;                                   /* align arrays */
  uint32_t argv_base = kseg0(pa);
  for(uint32_t p : argptrs) put_be32(p);
  put_be32(0);                                            /* argv NULL terminator */
  uint32_t envp_base = kseg0(pa);
  for(uint32_t p : envptrs) put_be32(p);
  put_be32(0);                                            /* envp NULL terminator */

  s->gpr[4] = argc;                 /* a0 = argc */
  s->gpr[5] = sext32(argv_base);    /* a1 = argv */
  s->gpr[6] = sext32(envp_base);    /* a2 = envp */
}
