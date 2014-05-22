#include <list>

#ifndef __LOAD_ELF_H
#define __LOAD_ELF_H

void load_elf(const char* fn, 
	      uint32_t *entry_p,
	      uint32_t *last_a,
	      std::list<std::pair<uint32_t, uint32_t> > &segs,
	      uint8_t *mem);

#endif 

