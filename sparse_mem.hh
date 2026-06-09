#ifndef __sparse_mem_hh__
#define __sparse_mem_hh__

#include <cstdlib>
#include <cassert>
#include <cstring>
#include <cstdint>


#include <map>

#include "sim_bitvec.hh"
#include "helper.hh"

#ifndef unlikely
#define unlikely(x)    __builtin_expect(!!(x), 0)
#endif

struct state_t;
class sgi_mc;
class sgi_hpc;

class sparse_mem {
public:
  static const uint64_t pgsize = 4096;
  static const uint64_t sz = 1UL<<32;  
  uint8_t *mem = nullptr;
  state_t *st = nullptr;
  bool route_devices = false;
public:
  sparse_mem();
  ~sparse_mem();
  void clear();

  uint8_t * operator[](uint64_t addr) {
    return &mem[addr];
  }
  uint8_t * operator+(uint64_t disp) {
    return (*this)[disp];
  }
  bool compare(const sparse_mem &other, bool verbose = false) {
    bool error = false;
    for(uint64_t b = 0; b < sz; ++b) {
      if(mem[b] != other.mem[b]) {
	error = true;
	if(verbose) {
	  std::cout << "byte " << std::hex << b
		    << " differs "
		    << static_cast<int>(mem[b])
		    << " vs "
		    << static_cast<int>(other.mem[b])
		    << std::dec
		    << "\n";
	}
      }
    }
    return error;
  }
  uint8_t *get_raw_ptr(uint64_t byte_addr) {
    byte_addr &= ((1UL<<32) - 1);
    return mem+byte_addr;
  }
  template <typename T>
  T get(uint64_t byte_addr) {
    if(route_devices && byte_addr>=0x1f000000 && byte_addr<=0x1fffffff) return route_load<T>(byte_addr);
    return *reinterpret_cast<T*>(mem+byte_addr);
  }
  template<typename T>
  void set(uint64_t byte_addr, T v) {
    //static_assert(sizeof(T) != 8);
    if(route_devices && byte_addr>=0x1f000000 && byte_addr<=0x1fffffff) { route_store<T>(byte_addr, v); return; }
    *reinterpret_cast<T*>(mem+byte_addr) = v;
  }
  template <typename T> T route_load(uint64_t pa);
  template <typename T> void route_store(uint64_t pa, T v);
  uint64_t bytes_allocated() const {
    return sz;
  }
};



#endif
