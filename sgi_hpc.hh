#ifndef __sgi_hpc__
#define __sgi_hpc__

#include <cstdint>
#include <cstddef>

struct state_t;

class sgi_hpc {
  state_t *s;
  uint32_t intstat;
  uint32_t misc;
public:
  sgi_hpc(state_t *s) : s(s), intstat(0), misc(0) {}
  uint32_t read(uint32_t offs, size_t sz);
  void write(uint32_t offs, uint32_t x, size_t sz);  
};


#endif
