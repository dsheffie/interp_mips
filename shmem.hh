#ifndef __DBS_SHMEM__
#define __DBS_SHMEM__

#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <semaphore.h>
#include <errno.h>

#define SHM_BYTES (1<<20)
#define SHM_KEY 0xDEADBEEE
#define NUM_SEMA 2
#define NUM_LOCKS 16
class shmem
{
 public:
  char errBuf[256];
  shmem(size_t numBytes,key_t key);
  ~shmem();
  sem_t *semaPtr[NUM_SEMA];
  int *lockPtrs[NUM_LOCKS];
  void lock(int lck_id);
  void unlock(int lck_id);
  void setupAddr();

  void semaDown(int id);
  void semaUp(int id);	

  void fastSemaDown(int lck_id);
  void fastSemaUp(int lck_id);	

  void printSemaphores();

  void pushInt(int v);
  void pushFloat(float f);
  
  void pushArray(void *arr, int num_elem);
  void *popArray(int &num_elem);
  void popArray(void *buf);

  int popInt();
  float popFloat();
  
  int checkQuit();
  void setQuit();

  int stackDepth();

  key_t key;
  int shmid;
  int *byteCnt;
  int *quitFlag;
  size_t numBytes;
  char *memPtr;
  char *dataPtr;
};

class shmClient : public shmem
{
 public: 
  shmClient(size_t numBytes, key_t key);
  ~shmClient();
};

class shmServer : public shmem
{
 public:
  shmServer(size_t numBytes,key_t key);
  void waitForConnection();
  ~shmServer();
};


#endif
