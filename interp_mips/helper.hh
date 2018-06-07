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


#ifndef print_var
#define print_var(x) std::cout <<  #x << " = " << x << "\n";
#endif

double timestamp();

uint32_t update_crc(uint32_t crc, uint8_t *buf, size_t len);
uint32_t crc32(uint8_t *buf, size_t len);

int32_t remapIOFlags(int32_t flags);

template <class T>
std::string toString(T x) {
  return std::to_string(x);
}

template <class T>
std::string toStringHex(T x) {
  std::stringstream ss;
  ss << std::hex << x;
  return ss.str();
}

#define INTEGRAL_ENABLE_IF(SZ,T) typename std::enable_if<std::is_integral<T>::value and (sizeof(T)==SZ),T>::type* = nullptr

template <typename T, INTEGRAL_ENABLE_IF(1,T)>
T accessBigEndian(T x) {
  return x;
}

template <typename T, INTEGRAL_ENABLE_IF(2,T)> 
T accessBigEndian(T x) {
  static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "must be little endian machine");
#ifdef MIPSEL
  return x;
#else  
  return  __builtin_bswap16(x);
#endif
}

template <typename T, INTEGRAL_ENABLE_IF(4,T)>
T accessBigEndian(T x) {
  static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "must be little endian machine");
#ifdef MIPSEL
  return x;
#else
  return  __builtin_bswap32(x);
#endif
}

template <typename T, INTEGRAL_ENABLE_IF(8,T)> 
T accessBigEndian(T x) {
  static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "must be little endian machine");
#ifdef MIPSEL
  return x;
#else
  return  __builtin_bswap64(x);
#endif
}

#undef INTEGRAL_ENABLE_IF



template <class T> bool isPow2(T x) {
  return (((x-1)&x) == 0);
}


#endif
