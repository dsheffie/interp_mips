#ifndef __sgi_indy__
#define __sgi_indy__

#include <cstdint>
#include <iostream>
#include <fstream>

#include "sgi_mc.hh"
#include "sgi_hpc.hh"

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

static const std::map<mem_range_t, std::string> rangeNames = {
  {mem_range_t::sys_mem_alias, "sys_mem_alias"},
  {mem_range_t::eisa_io, "eisa_io"},
  {mem_range_t::eisa_io_alias, "eisa_io_alias"},
  {mem_range_t::eisa_mem_128M, "eisa_mem_128M"},
  {mem_range_t::low_local, "low_local"},
  {mem_range_t::reserved, "reserved"},
  {mem_range_t::graphics, "graphics"},
  {mem_range_t::gio64_slot0, "gio64_slot0"},
  {mem_range_t::gio64_slot1, "gio64_slot1"},
  {mem_range_t::mc_regs, "mc_regs"},
  {mem_range_t::hpc_regs, "hpc_regs"},
  {mem_range_t::boot_rom, "boot_rom"},
  {mem_range_t::high_local, "high_local"},
  {mem_range_t::eisa_mem_2048M, "eisa_mem_2048M"}
};

static inline std::ostream &operator <<(std::ostream &out, const mem_range_t &mr) {
  out << rangeNames.at(mr);
  return out;
}



#endif
