#ifndef __HELPERFUNCTS__
#define __HELPERFUNCTS__
#include <string>
#include <sstream>
#include <vector>
#include <cstdint>
#include <iostream>

#define KNRM  "\x1B[0m"
#define KRED  "\x1B[31m"
#define KGRN  "\x1B[32m"
#define KYEL  "\x1B[33m"
#define KBLU  "\x1B[34m"
#define KMAG  "\x1B[35m"
#define KCYN  "\x1B[36m"
#define KWHT  "\x1B[37m"

void dbt_backtrace();

#define die() {								\
    std::cerr << __PRETTY_FUNCTION__ << " @ " << __FILE__ << ":"	\
	      << __LINE__ << " called die\n";				\
    dbt_backtrace();							\
    abort();								\
  }


double timestamp();

uint32_t update_crc(uint32_t crc, uint8_t *buf, size_t len);
uint32_t crc32(uint8_t *buf, size_t len);

int32_t remapIOFlags(int32_t flags);

template <class T>
std::string toString(T x) {
  std::stringstream ss;
  ss << x;
  return ss.str();
}

template <class T>
std::string toStringHex(T x) {
  std::stringstream ss;
  ss << std::hex << x;
  return ss.str();
}

template <typename T>
T accessBigEndian(T x) {
#ifdef MIPSEL
   return x;
#else
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
#endif
}

template <class T> bool isPow2(T x) {
  return (((x-1)&x) == 0);
}


#endif
