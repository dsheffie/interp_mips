#ifndef __SIM_CACHE_H__
#define __SIM_CACHE_H__
#include <string>
#include <sstream>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>
#include <cassert>
#include <stdint.h>

enum opType {READ=0,WRITE};

class simCache
{
 public:
   simCache(size_t bytes_per_line, size_t assoc, size_t num_sets, 
	    std::string name, int latency, simCache *next_level);
   ~simCache();

   void set_next_level(simCache *next_level);

   uint32_t index(uint32_t addr, uint32_t &l, uint32_t &t);
   virtual void access(uint32_t addr, uint32_t num_bytes, opType o)=0;
 
   void read(uint32_t addr, uint32_t num_bytes);
   void write(uint32_t addr, uint32_t num_bytes);
   std::string getStats(std::string &fName);
   void getStats();
   double computeAMAT();
   
   /* cache size stats */
   size_t bytes_per_line;
   size_t num_sets;
   size_t assoc;
   size_t total_cache_size;
   
   size_t ln2_tag_bits;
   size_t ln2_offset_bits;
   size_t ln2_num_sets;
   size_t ln2_bytes_per_line;
   
   /* total stats */
   size_t hits,misses;
  /* read/write stats */
   size_t rw_hits[2],rw_misses[2];
   
   /* next level cache */
   simCache *next_level;
  /* latency */
   int latency;
   
   std::string name;
  
};

class randomReplacementCache : public simCache
{
 public:
  randomReplacementCache(size_t bytes_per_line, size_t assoc, size_t num_sets, 
		 std::string name, int latency, simCache *next_level);
  ~randomReplacementCache();
  virtual void access(uint32_t addr, uint32_t num_bytes, opType o);

 private:
  /* all bits are valid */
  uint8_t *allvalid;
  /* cache valid bits */
   uint8_t **valid;
   /* cache tag bits */
   uint32_t **tag;
};


class realLRUCache : public simCache
{
 public:
  realLRUCache(size_t bytes_per_line, size_t assoc, size_t num_sets, 
		 std::string name, int latency, simCache *next_level);
  ~realLRUCache();
  virtual void access(uint32_t addr, uint32_t num_bytes, opType o);

 private:
  /* all bits are valid */
  uint8_t *allvalid;
  /* cache valid bits */
   uint8_t **valid;
   /* tree-lru bits */
   uint64_t **lru;
   /* cache tag bits */
   uint32_t **tag;
};

/* This is too expensive to be used in practice */
class highAssocCache : public simCache
{
 public:
  highAssocCache(size_t bytes_per_line, size_t assoc, size_t num_sets, 
		 std::string name, int latency, simCache *next_level);
  ~highAssocCache();
  virtual void access(uint32_t addr, uint32_t num_bytes, opType o);

 private:
  void updateLRU(uint32_t idx,uint32_t w);
  int32_t findLRU(uint32_t w);
  
  uint8_t *allvalid;
  /* cache valid bits */
   uint8_t **valid;
   
  /* tree-lru bits */
   uint8_t **lru;
  
   /* cache tag bits */
   uint32_t **tag;

};

class lowAssocCache : public simCache
{
 public:
  lowAssocCache(size_t bytes_per_line, size_t assoc, size_t num_sets, 
		 std::string name, int latency, simCache *next_level);
  ~lowAssocCache();
  virtual void access(uint32_t addr, uint32_t num_bytes, opType o);

 private:
  void updateLRU(uint32_t idx,uint32_t w);
  int32_t findLRU(uint32_t w);
  
  uint8_t *allvalid;

  /* cache valid bits */
   uint64_t *valid;
   
  /* tree-lru bits */
   uint64_t *lru;
  
   /* cache tag bits */
   uint32_t **tag;

};


#endif



