#ifndef __HELPERFUNCTS__
#define __HELPERFUNCTS__
#include <string>
#include <sstream>
#include <vector>
#include <stdint.h>


double timestamp();
void md5sum(uint8_t *bytes, size_t nBytes, std::vector<uint8_t> &hash);

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

#endif
