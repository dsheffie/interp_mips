#ifndef __HELPERFUNCTS__
#define __HELPERFUNCTS__
#include <string>
#include <sstream>
#include <vector>
#include <stdint.h>

#define KNRM  "\x1B[0m"
#define KRED  "\x1B[31m"
#define KGRN  "\x1B[32m"
#define KYEL  "\x1B[33m"
#define KBLU  "\x1B[34m"
#define KMAG  "\x1B[35m"
#define KCYN  "\x1B[36m"
#define KWHT  "\x1B[37m"

double timestamp();
inline uint64_t rdtsc(void) 
{
  uint32_t hi=0, lo=0;
#ifdef __amd64__
  __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
#endif
  return ( (uint64_t)lo)|( ((uint64_t)hi)<<32 );
}
uint32_t update_crc(uint32_t crc, uint8_t *buf, size_t len);
uint32_t crc32(uint8_t *buf, size_t len);

int32_t remapIOFlags(int32_t flags);

#define likely(x)    __builtin_expect (!!(x), 1)
#define unlikely(x)  __builtin_expect (!!(x), 0)

template <class T> std::string toString(T x)
{
  std::stringstream ss;
  ss << x;
  return ss.str();
}

template <class T> std::string toStringHex(T x)
{
  std::stringstream ss;
  ss << std::hex << x;
  return ss.str();
}

template <class T> T accessBigEndian(T x)
 {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  if(sizeof(x) == 1)
    return x;
  else if(sizeof(x) == 2)
    return  __builtin_bswap16(x);
  else if(sizeof(x) == 4)
    return __builtin_bswap32(x);
  else 
    return __builtin_bswap64(x);
#else
  return x;
#endif
}

template <class T> bool isPow2(T x)
{
  return (((x-1)&x) == 0);
}

class onlineStat
{
private:
  double M,S;
  uint64_t nSamples;
public:
  onlineStat() {
    S = M = 0.0;
    nSamples = 0;
  }
  void update(double sample) {
    nSamples++;
    if(nSamples == 1) {
      M = sample;
      S = 0;
    } else {
      double nM = M + ((sample - M) / (double)(nSamples));
      double nS = S + ((sample - M) * (sample - nM));
      M = nM;
      S = nS;
    }
  }
  double mean() {
    return M;
  }
  double var() {
    return (nSamples==0) ? 0.0 : (S / ((double)(nSamples-1)));
  }
};

#endif
