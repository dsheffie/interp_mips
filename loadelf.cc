#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cassert>
#include <utility>
#include <cstdint>
#include <list>


#include "helper.hh"
#include "interpret.hh"
#include "globals.hh"

#ifdef __APPLE__
#include "TargetConditionals.h"
#ifdef TARGET_OS_MAC
#include "osx_elf.h"
#endif
#else
#include <elf.h>
#endif


#define INTEGRAL_ENABLE_IF(SZ,T) typename std::enable_if<std::is_integral<T>::value and (sizeof(T)==SZ),T>::type* = nullptr

template <typename T, INTEGRAL_ENABLE_IF(1,T)>
T bswap_(T x) {
  return x;
}

template <typename T, INTEGRAL_ENABLE_IF(2,T)> 
T bswap_(T x) {
  static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "must be little endian machine");
  return  __builtin_bswap16(x);
}

template <typename T, INTEGRAL_ENABLE_IF(4,T)>
T bswap_(T x) {
  static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "must be little endian machine");
  return  __builtin_bswap32(x);
}

template <typename T, INTEGRAL_ENABLE_IF(8,T)> 
T bswap_(T x) {
  static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "must be little endian machine");
  return  __builtin_bswap64(x);
}

#undef INTEGRAL_ENABLE_IF


static const uint8_t magicArr[4] = {0x7f, 'E', 'L', 'F'};
bool checkElf(const Elf32_Ehdr *eh32) {
  uint8_t *identArr = (uint8_t*)eh32->e_ident;
  return memcmp((void*)magicArr, identArr, 4)==0;
}

bool check32Bit(const Elf32_Ehdr *eh32) {
  return (eh32->e_ident[EI_CLASS] == ELFCLASS32);
}

bool checkBigEndian(const Elf32_Ehdr *eh32) {
  return (eh32->e_ident[EI_DATA] == ELFDATA2MSB);
}

bool checkLittleEndian(const Elf32_Ehdr *eh32) {
  return (eh32->e_ident[EI_DATA] == ELFDATA2LSB);
}

void load_elf(const char* fn, state_t *ms) {
  struct stat s;
  Elf32_Ehdr *eh32 = nullptr;
  Elf32_Phdr* ph32 = nullptr;
  Elf32_Shdr* sh32 = nullptr;
  int32_t e_phnum=-1,e_shnum=-1,e_phoff=-1;
  size_t pgSize = getpagesize();
  int fd,rc;
  char *buf = nullptr;
  sparse_mem &mem = ms->mem;

  ms->mem.clear();
  
  fd = open(fn, O_RDONLY);
  if(fd<0) {
    printf("INTERP: open() returned %d\n", fd);
    exit(-1);
  }
  rc = fstat(fd,&s);
  if(rc<0) {
    printf("INTERP: fstat() returned %d\n", rc);
    exit(-1);
  }
  buf = (char*)mmap(nullptr, s.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  eh32 = (Elf32_Ehdr *)buf;
  close(fd);
    
  if(!checkElf(eh32) || !check32Bit(eh32)) {
    printf("INTERP: Bogus binary\n");
    exit(-1);
  }

  /* Check for a MIPS machine */
  assert(checkBigEndian(eh32));

  if(bswap_(eh32->e_machine) != 8) {
    printf("INTERP : non-mips binary..goodbye\n");
    exit(-1);
  }

  uint32_t lAddr = bswap_(eh32->e_entry);

  e_phnum = bswap_(eh32->e_phnum);
  e_phoff = bswap_(eh32->e_phoff);
  ph32 = reinterpret_cast<Elf32_Phdr*>(buf + e_phoff);
  e_shnum = bswap_(eh32->e_shnum);
  sh32 = reinterpret_cast<Elf32_Shdr*>(buf + bswap_(eh32->e_shoff));
  ms->pc = (int64_t)(int32_t)lAddr;

  uint32_t loaddr = ~0U, hiaddr = 0;
  /* Find instruction segments and copy to
   * the memory buffer */
  for(int32_t i = 0; i < e_phnum; i++, ph32++) {
    int32_t p_memsz = bswap_(ph32->p_memsz);
    int32_t p_offset = bswap_(ph32->p_offset);
    int32_t p_filesz = bswap_(ph32->p_filesz);
    int32_t p_type = bswap_(ph32->p_type);
    uint32_t p_vaddr = bswap_(ph32->p_vaddr);
    /* stolen from QEMU */
    if(p_type == PT_LOAD) {
      uint32_t sz = (p_vaddr + p_memsz);
      loaddr = std::min(loaddr, (p_vaddr - p_offset));
      hiaddr = std::max(hiaddr, sz);
    }
    uint32_t vaddr_ef = p_vaddr + p_filesz;
    uint32_t vaddr_em = p_vaddr + p_memsz;
    
    if(p_type == SHT_PROGBITS && p_memsz) {
      if( (p_vaddr + p_memsz) > lAddr) {
        lAddr = (p_vaddr + p_memsz);
      }
      // std::cerr << "loading ELF segment at virtual address "
      // 		<< std::hex << p_vaddr << std::dec << " of memsz "
      // 		<< p_memsz << " and filesz "
      // 		<< p_filesz
      // 		<< " end virtual address "
      // 		<< std::hex
      // 		<< (p_vaddr + p_memsz)
      // 		<< std::dec
      // 		<< "\n";
      
      
      /* not strictly required, prefault fills with zeros */
      for(int32_t cc = 0; cc < p_memsz; cc++) {
	mem.set<uint8_t>(cc+p_vaddr, 0);
      }
      
      for(int32_t cc = 0; cc < p_filesz; cc++) {
	uint32_t v_addr = cc+p_vaddr;
	mem.set<uint8_t>(va2pa(v_addr),
			 reinterpret_cast<uint8_t*>(buf + p_offset)[cc]);
      }
    }
    
  }
  /* again, stolen from QEMU */
  munmap(buf, s.st_size);
  //exit(-1);
}

