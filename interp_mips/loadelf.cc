#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <assert.h>
#include <utility>

#include <list>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>

#include "helper.hh"

#ifdef __APPLE__
#include "TargetConditionals.h"
#ifdef TARGET_OS_MAC
#include "osx_elf.h"
#endif
#else
#include <elf.h>
#endif

bool checkElf(const Elf32_Ehdr *eh32)
{
  uint8_t magicArr[4] = {0x7f, 'E', 'L', 'F'};
  uint8_t *identArr = (uint8_t*)eh32->e_ident;

  for(int i = 0; i < 4; i++)
    {
      if(identArr[i] != magicArr[i])
	return false;
    }
  return true;
}

bool check32Bit(const Elf32_Ehdr *eh32)
{
  return (eh32->e_ident[EI_CLASS] == ELFCLASS32);
}

bool checkBigEndian(const Elf32_Ehdr *eh32)
{
  return (eh32->e_ident[EI_DATA] == ELFDATA2MSB);
}


void load_elf(const char* fn,
	      uint32_t *entry_p,
	      uint32_t *last_a,
	      std::list<std::pair<uint32_t, uint32_t> > &segs,
	      uint8_t *mem)
{
  struct stat s;
  Elf32_Ehdr *eh32 = 0;
  Elf32_Phdr* ph32 = 0;
  Elf32_Shdr* sh32 = 0;
  size_t pgSize = getpagesize();
  int fd,rc;
  fd = open(fn, O_RDONLY);
  if(fd<0) {
    printf("JIT: open() returned %d\n", fd);
    exit(-1);
  }
  rc = fstat(fd,&s);
  if(rc<0) {
    printf("JIT: fstat() returned %d\n", rc);
    exit(-1);
  }
  char* buf = (char*)mmap(NULL, s.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  assert(buf != MAP_FAILED);
  close(fd);
  
  eh32 = (Elf32_Ehdr *)buf;
  
  if(!checkElf(eh32)) {
    printf("not an elf binary\n");
    abort();
  }

  if(!check32Bit(eh32)) {
    printf("not a 32-bit binary\n");
    abort();
  }
  if(!checkBigEndian(eh32)) {
    printf("not a big-endian binary..goodbye\n");
    abort();
  }

  /* MIPS */
  if(accessBigEndian(eh32->e_machine) != 8) {
    printf("non-mips binary..goodbye\n");
    abort();
  }
  *entry_p = accessBigEndian(eh32->e_entry);

  /*printf("e_phoff = %d\n", accessBigEndian(eh32->e_phoff));*/

  int32_t e_phnum = accessBigEndian(eh32->e_phnum);
  /*printf("e_phnum = %d\n", e_phnum); */
  ph32 = (Elf32_Phdr*)(buf + accessBigEndian(eh32->e_phoff));
  assert(ph32);

  int32_t e_shnum = accessBigEndian(eh32->e_shnum);
  sh32 = (Elf32_Shdr*)(buf + accessBigEndian(eh32->e_shoff));
  uint32_t lAddr = *entry_p;

  for(int32_t i = 0; i < e_phnum; i++, ph32++)
    {
      int32_t p_memsz = accessBigEndian(ph32->p_memsz);
      int32_t p_offset = accessBigEndian(ph32->p_offset);
      int32_t p_filesz = accessBigEndian(ph32->p_filesz);
      int32_t p_type = accessBigEndian(ph32->p_type);
      int32_t p_flags = accessBigEndian(ph32->p_flags);
      uint32_t p_vaddr = accessBigEndian(ph32->p_vaddr);

      if(p_type == SHT_PROGBITS && p_memsz)
	{
	  std::pair<uint32_t, uint32_t> p(p_vaddr, p_memsz);
	  segs.push_back(p);

	  /*
	  printf("ph32 (%d) : va = %x, memsz=%d, offset=%d, filesz=%d\n", 
		 i, p_vaddr, p_memsz, p_offset, p_filesz);
	  */
	  if( (p_vaddr + p_memsz) > lAddr)
	    lAddr = (p_vaddr + p_memsz);

	  /* Zero out segment */
	  memset(mem+p_vaddr, 0, sizeof(uint8_t)*p_memsz);

	  memcpy(mem+p_vaddr, (uint8_t*)(buf + p_offset),
		 sizeof(uint8_t)*p_filesz);
	}
    }

  for(int32_t i = 0; i < e_shnum; i++, sh32++) {
    int32_t f = accessBigEndian(sh32->sh_flags);
    if(f & SHF_EXECINSTR) {
      uint32_t addr = accessBigEndian(sh32->sh_addr);
      int32_t size = accessBigEndian(sh32->sh_size);
      bool pgAligned = ((addr & 4095) == 0);
      if(pgAligned) {
	/* mprotect() will round up to the nearest
	 * page and the linker script puts stuff
	 * there by default */
	size = (size / pgSize) * pgSize;
	//printf("segment starting at %x of size %u, end = %x is .text\n",
	//addr, size, (addr+size));
	void *mpaddr = (void*)(mem+addr);
	rc = mprotect(mpaddr, size, PROT_READ);
	/* printf("mpaddr = %p, rc = %d\n", mpaddr, rc);*/
	if(rc != 0) {
	  int err = errno;
	  printf("mprotect rc = %d, error(%d) = %s\n", rc, 
		 err, strerror(err));
	}
      }
      //printf("pgAligned = %s\n", pgAligned ? "yes" : "no");
    }
  }


  *last_a = lAddr;
  munmap(buf, s.st_size);
}
