#ifndef __instrecord_hh__
#define __instrecord_hh__

/* NO_BOOST_SERIALIZATION: keep the types but drop the archive support, for builds
 * that never write a retire trace and do not have boost (e.g. wasm). */
#ifndef NO_BOOST_SERIALIZATION
#if BOOST_VERSION >= 107400
#include <boost/serialization/library_version_type.hpp>
#endif


#include <boost/serialization/serialization.hpp>
#include <boost/serialization/list.hpp>
#include <boost/serialization/map.hpp>
#include <boost/archive/binary_oarchive.hpp>
#include <boost/archive/binary_iarchive.hpp>
#include <boost/archive/text_oarchive.hpp>
#include <boost/archive/text_iarchive.hpp>
#endif
#include <cstdint>
#include <fstream>
#include <string>
#include <list>

struct inst_record {
  uint64_t pc;
  uint64_t vpc;
  uint32_t inst;
#ifndef NO_BOOST_SERIALIZATION
  friend class boost::serialization::access;
  template<class Archive>
  void serialize(Archive & ar, const unsigned int version) {
    ar & pc;
    ar & vpc;
    ar & inst;
  }
#endif
  inst_record(uint64_t pc, uint64_t vpc, uint32_t inst) :
    pc(pc), vpc(vpc), inst(inst) {}
  inst_record() : pc(0), vpc(0), inst(0) {}
};

class retire_trace {
public:
  std::list<inst_record> records;
  std::map<int64_t, double> tip;
#ifndef NO_BOOST_SERIALIZATION
  friend class boost::serialization::access;
  template<class Archive>
  void serialize(Archive & ar, const unsigned int version) {
    ar & records;
    ar & tip;
  }
#endif
  retire_trace() {}
  bool empty() const {
     return records.empty();
  }
  const std::list<inst_record> &get_records() const {
    return records;
  }
  std::list<inst_record> &get_records() {
    return records;
  }
};

#endif
