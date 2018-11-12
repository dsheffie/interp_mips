#ifndef __PROFILEMIPS__
#define __PROFILEMIPS__

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <ostream>

/* from gdb simulator */
#define RSVD_INSTRUCTION           (0x00000005)
#define RSVD_INSTRUCTION_MASK      (0xFC00003F)
#define RSVD_INSTRUCTION_ARG_SHIFT 6
#define RSVD_INSTRUCTION_ARG_MASK  0xFFFFF  
#define IDT_MONITOR_BASE           0xBFC00000
#define IDT_MONITOR_SIZE           2048
#define MARGS 20

#define K1SIZE  (0x80000000)

typedef struct {
  uint32_t tv_sec;
  uint32_t tv_usec;
} timeval32_t;

typedef struct {
  uint32_t tms_utime;
  uint32_t tms_stime;
  uint32_t tms_cutime;
  uint32_t tms_cstime;
} tms32_t;

typedef struct {
  uint16_t st_dev;
  uint16_t st_ino;
  uint32_t st_mode;
  uint16_t st_nlink;
  uint16_t st_uid;
  uint16_t st_gid;
  uint16_t st_rdev;
  uint32_t st_size;
  uint32_t _st_atime;
  uint32_t st_spare1;
  uint32_t _st_mtime;
  uint32_t st_spare2;
  uint32_t _st_ctime;
  uint32_t st_spare3;
  uint32_t st_blksize;
  uint32_t st_blocks;
  uint32_t st_spare4[2];
} stat32_t;

typedef struct {
  uint32_t pc;
  uint32_t last_pc;
  int32_t gpr[32];
  int32_t lo;
  int32_t hi;
  uint32_t cpr0[32];
  uint32_t cpr1[32];
  uint32_t fcr1[5];
  uint64_t icnt;
  uint8_t *mem;
  uint8_t brk;
  uint64_t maxicnt;
} state_t;

struct rtype_t {
  uint32_t opcode : 6;
  uint32_t sa : 5;
  uint32_t rd : 5;
  uint32_t rt : 5;
  uint32_t rs : 5;
  uint32_t special : 6;
};

struct itype_t {
  uint32_t imm : 16;
  uint32_t rt : 5;
  uint32_t rs : 5;
  uint32_t opcode : 6;
};

struct coproc1x_t {
  uint32_t fmt : 3;
  uint32_t id : 3;
  uint32_t fd : 5;
  uint32_t fs : 5;
  uint32_t ft : 5;
  uint32_t fr : 5;
  uint32_t opcode : 6;
};

struct lwxc1_t {
  uint32_t id : 6;
  uint32_t fd : 5;
  uint32_t pad : 5;
  uint32_t index : 5;
  uint32_t base : 5;
  uint32_t opcode : 6;
};

union mips_t {
  rtype_t r;
  itype_t i;
  coproc1x_t c1x;
  lwxc1_t lc1x;
  uint32_t raw;
  mips_t(uint32_t x) : raw(x) {}
};

void initState(state_t *s);
void execMipsEL(state_t *s);
void execMips(state_t *s);
void mkMonitorVectors(state_t *s);
std::ostream &operator<<(std::ostream &out, const state_t & s);
#endif
