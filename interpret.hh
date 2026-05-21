#ifndef __INTERPRET_HH__
#define __INTERPRET_HH__

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <ostream>

#include "sgi_mc.hh"
#include "sgi_hpc.hh"

/* from gdb simulator */
#define RSVD_INSTRUCTION           (0x00000005)
#define RSVD_INSTRUCTION_MASK      (0xFC00003F)
#define RSVD_INSTRUCTION_ARG_SHIFT 6
#define RSVD_INSTRUCTION_ARG_MASK  0xFFFFF  
#define IDT_MONITOR_BASE           0xBFC00000
#define IDT_MONITOR_SIZE           2048
#define MARGS 20

#define K1SIZE  (0x80000000)

struct timeval32_t {
  uint32_t tv_sec;
  uint32_t tv_usec;
};

struct tms32_t { 
  uint32_t tms_utime;
  uint32_t tms_stime;
  uint32_t tms_cutime;
  uint32_t tms_cstime;
};

struct stat32_t {
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
};

struct state_t{
  uint32_t pc;
  uint32_t last_pc;
  int32_t gpr[32];
  int32_t lo;
  int32_t hi;
  uint32_t cpr0[32];
  uint32_t cpr1[32];
  uint32_t fcr1[5];
  uint64_t icnt;
  uint64_t nopcnt;
  uint8_t *mem;
  sgi_mc *mc;
  sgi_hpc *hpc;
  uint8_t brk;
  uint64_t maxicnt;
};

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

enum class mem_range_t {sys_mem_alias,
			eisa_io,
			eisa_io_alias,
			eisa_mem_128M,
			low_local,
			reserved,
			graphics,
			gio64_slot0,
			gio64_slot1,
			mc_regs,
			hpc_regs,
			boot_rom,
			high_local,
			eisa_mem_2048M};

inline static mem_range_t compute_mem_range_type(uint32_t pa) {
  if(pa <= 0x7ffff) {
    return mem_range_t::sys_mem_alias;
  }
  else if(pa >= 0x80000 and pa <= 0x0008ffff) {
    return mem_range_t::eisa_io;
  }
  else if(pa >= 0x00090000 and pa <= 0x0009ffff) {
    return mem_range_t::eisa_io_alias;
  }
  else if(pa >= 0x000a0000 and pa <= 0x07ffffff) {
    return mem_range_t::eisa_mem_128M;
  }
  else if(pa >= 0x08000000 and pa <= 0x17ffffff) {
    return mem_range_t::low_local;
  }
  else if(pa >= 0x1f000000 and pa <= 0x1f3fffff) {
    return mem_range_t::graphics;
  }
  else if(pa >= 0x1f400000 and pa <= 0x1f5fffff) {
    return mem_range_t::gio64_slot0;
  }
  else if(pa >= 0x1f600000 and pa <= 0x1f9fffff) {
    return mem_range_t::gio64_slot1;
  }
  else if(pa >= 0x1fa00000 and pa <= 0x1fafffff) {
    return mem_range_t::mc_regs;
  }
  else if(pa >= 0x1fb00000 and pa <= 0x1fbfffff) {
    return mem_range_t::hpc_regs;
  }
  else if(pa >= 0x1fc00000 and pa <= 0x1fffffff) {
    return mem_range_t::boot_rom;
  }
  else if(pa >= 0x20000000 and pa <= 0x2fffffff) {
    return mem_range_t::high_local;
  }
  else if(pa >= 0x80000000 and pa <= 0xffffffff) {
    return mem_range_t::eisa_mem_2048M;
  }
  return mem_range_t::reserved;
}

#endif
