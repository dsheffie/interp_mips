#include <string>
#include <sstream>
#include <vector>
#include <stdint.h>
#include <sys/time.h>
#include <time.h>
#include <mhash.h>

double timestamp()
{
  struct timeval t;
  gettimeofday(&t,NULL);
  return t.tv_sec + 1e-6*t.tv_usec;
}

void md5sum(uint8_t *bytes, size_t nBytes, std::vector<uint8_t> &hash)
{
  uint8_t uhash[16]; 
  size_t stripBound = nBytes - (nBytes % (1<<20));
  MHASH td = mhash_init(MHASH_MD5);
  for(size_t i = 0; i < stripBound; i+=(1<<20))
    {
      mhash(td, bytes+i, (1<<20));
    }
  for(size_t i = stripBound; i < nBytes; i++)
    {
      mhash(td, bytes+i, 1);
    }
  mhash_deinit(td, uhash);
  for (int32_t i = 0; i < mhash_get_block_size(MHASH_MD5); i++)
    {
      hash.push_back(uhash[i]);
    }
}
