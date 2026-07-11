#include "shmem.hh"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <unistd.h>



shmem::shmem(size_t numBytes,key_t key)
{
  this->key = key;
  this->numBytes = numBytes;
  memPtr = NULL;
  byteCnt = NULL;
}

shmem::~shmem()
{

}


void shmem::setQuit()
{
  *quitFlag = 1;
}

int shmem::checkQuit()
{
  return *quitFlag;
}

void shmem::semaDown(int id)
{
  int rc;
  assert(id >= 0 && id < NUM_SEMA);
  int value;
  sem_getvalue(semaPtr[id], &value);
  //printf("semaDown(%d): value = %d\n",id, value);
  rc = sem_wait(semaPtr[id]);
  assert(rc == 0);
}

void shmem::semaUp(int id)
{
  int rc;
  assert(id >= 0 && id < NUM_SEMA);
  int value;
  sem_getvalue(semaPtr[id], &value);
  //printf("semaUp(%d): value = %d\n",id, value);
  rc = sem_post(semaPtr[id]);
  assert(rc == 0);
}

void shmem::pushInt(int v)
{
  int idx = __sync_fetch_and_add(byteCnt,sizeof(int));
  int *ptr = (int*)(dataPtr+idx);
  *ptr = v;
}

int shmem::stackDepth()
{
  return *byteCnt;
}

void shmem::pushFloat(float f)
{
  int idx =  __sync_fetch_and_add(byteCnt,sizeof(float));
  float *ptr = (float*)(dataPtr+idx);
  *ptr = f;
}

void shmem::pushArray(void *arr, int num_elem)
{
  int idx = *byteCnt;
  void *ptr = (void*)(dataPtr+idx);
  if((idx + num_elem + 3*sizeof(int)+NUM_SEMA*sizeof(sem_t))
     >= numBytes)
    {
      printf("can't hold array\n");
      return;
    } 

  memcpy(ptr,arr,sizeof(int)*num_elem);
  *byteCnt += num_elem*sizeof(int);
  pushInt(num_elem);
}

void shmem::popArray(void *buf)
{
  int numWord = popInt();
  //printf("numWord %d\n", numWord);
  int bc = __sync_fetch_and_sub(byteCnt,(numWord)*sizeof(int));
  int idx = bc - numWord*sizeof(int);
  assert(idx >= 0);
  void *ptr = (void*)(dataPtr+idx);
  float *fptr = (float*)ptr;
  //printf("fptr=%f,numWord=%d\n", *fptr, numWord);
  assert(numWord >= 0);
  //printf("buf = %p,ptr=%p\n", buf,ptr);
  memcpy(buf,ptr,sizeof(int)*numWord);
 
}

void *shmem::popArray(int &num_elem)
{
  int *retArray = NULL;
  int numWord = popInt();
  int bc = __sync_fetch_and_sub(byteCnt,(numWord)*sizeof(int));
  int idx = bc - numWord*sizeof(int);
  void *ptr = (void*)(dataPtr+idx);
  num_elem = numWord;
  assert(numWord >= 0);
  retArray = new int[numWord];
  memcpy(retArray,ptr,sizeof(int)*numWord);
  return retArray;
}

int shmem::popInt()
{
  int bc = __sync_fetch_and_sub(byteCnt,sizeof(int));
  int idx = bc - sizeof(int);
  assert(idx >= 0);
  int *ptr = (int*)(dataPtr + idx);
  return *ptr;
}

float shmem::popFloat()
{ 
  int bc = __sync_fetch_and_sub(byteCnt,sizeof(float));
  int idx = bc - sizeof(float);
  assert(idx >= 0);
  float *ptr = (float*)(dataPtr + idx);
  return *ptr;
}

void shmem::printSemaphores()
{
  for(int i = 0;i<2;i++)
    {
      int value;
      sem_getvalue(semaPtr[i], &value);
      printf("semaphore(%d)=%d\n", i, value);
    }
}

void shmem::setupAddr()
{
  
  for(int i = 0; i < NUM_SEMA; i++)
    {
      semaPtr[i] = (sem_t*)(memPtr+i*sizeof(sem_t));
    }
 
  for(int i=0;i<2;i++)
    {
      int value;
      sem_getvalue(semaPtr[i], &value);
      printf("semaphore %d has init value of %d\n",
	     i, value);
    }
  byteCnt = (int*)(memPtr+NUM_SEMA*sizeof(sem_t));
  quitFlag = (int*)(memPtr+NUM_SEMA*sizeof(sem_t)+sizeof(int));
  int *lckPtr = (int*)(memPtr + NUM_SEMA*sizeof(sem_t) + 2*sizeof(int));
  for(int i = 0; i < NUM_LOCKS; i++)
    {
      lockPtrs[i] = &lckPtr[i];
    }
  dataPtr = memPtr + NUM_SEMA*sizeof(sem_t) + 2*sizeof(int) + NUM_LOCKS*sizeof(int);
}

void shmem::fastSemaDown(int lck_id)
{
  assert(lck_id < NUM_LOCKS);
  volatile int *lck = lockPtrs[lck_id];
  bool success;
  do
    {
      success = __sync_bool_compare_and_swap(lck,1,0);
    } while(!success);
}
void shmem::fastSemaUp(int lck_id)
{
  assert(lck_id < NUM_LOCKS);
  volatile int *lck = lockPtrs[lck_id];
  bool success;
  do
    {
      success = __sync_bool_compare_and_swap(lck,0,1);
    } while(!success);
}

void shmem::lock(int lck_id)
{
  assert(lck_id < NUM_LOCKS);
  int *lck = lockPtrs[lck_id];
  int l;
  do
    {
      l = __sync_val_compare_and_swap(lck,0,1);
    } 
  while(l == 1);
}

void shmem::unlock(int lck_id)
{
  assert(lck_id < NUM_LOCKS);
  int *lck = lockPtrs[lck_id];
  *lck = 0;
  __sync_synchronize();
}



shmClient::shmClient(size_t numBytes,key_t key)
  : shmem(numBytes,key)
{
  shmid =  shmget(key, numBytes, 0666);
  if(shmid < 0)
    {
      const char *errStr = strerror(errno);
      printf("shmget() error %s\n", errStr);
      exit(-1);
    }
  memPtr = (char*)shmat(shmid, NULL, 0);
  if(memPtr == ((char*)-1))
    {
      printf("memPtr = %p\n", memPtr);
      exit(-1);
    }  

  setupAddr();
}

shmClient::~shmClient()
{
  shmdt(memPtr);
}


shmServer::shmServer(size_t numBytes,key_t key)
  : shmem(numBytes,key)
{
  int rc;
  shmid = shmget(key, numBytes, IPC_CREAT | 0666);
  if(shmid < 0)
    { 
      printf("shmget() error %s\n", strerror(errno));
      exit(-1);
    }
  memPtr = (char*)shmat(shmid, NULL, 0);
  memset(memPtr, 0, numBytes);

  if(memPtr == ((char*)-1))
    {
      printf("memPtr = %p\n", memPtr);
      exit(-1);
    }
  setupAddr();
  
  for(int i = 0; i < NUM_SEMA; i++)
    {
      rc = sem_init(semaPtr[i], 1, 0);
      assert(rc == 0); 
    } 
  *quitFlag = 0;
  *byteCnt = 0;
}

void shmServer::waitForConnection()
{
  bool conn;
  int rc;
  struct shmid_ds stat;
  do
    {
      conn = false;
      memset(&stat, 0, sizeof(struct shmid_ds));
      rc = shmctl(shmid, IPC_STAT, &stat); 
      if(rc==-1)
	{
	  printf("waitForConnection flagged error\n");
	  exit(-1);
	}
      conn = (stat.shm_nattch > 1);
      if(conn == false)
	sleep(1);

    } while(conn == false);

}


shmServer::~shmServer()
{
  struct shmid_ds stat;
  int rc;
  shmdt(memPtr);
  memset(&stat, 0, sizeof(struct shmid_ds));
  rc=shmctl(shmid,IPC_RMID,&stat);
  if(rc!=0)
    {
      const char *errStr = strerror(errno);
      printf("shmctl() error %s\n", errStr);
    }
}

#ifdef TST_CLIENT
int main()
{
  shmClient *client = new shmClient();
  client->semaUp(0);
  client->semaDown(1);
  delete client;
  return 0;
}
#endif

#ifdef TST_SERV
int main()
{
  int array[10];
  for(int i=0;i<10;i++)
    array[i] = i;

  shmServer *serv = new shmServer();
  printf("created server\n");

  serv->pushArray(array,10);

  // serv->pushInt(2);
  //serv->pushInt(3);
  
  serv->semaDown(0);

  printf("got connection\n");
  serv->semaUp(1);
  
  serv->semaDown(0);
  
  int num;
  int *yarra = (int*)serv->popArray(&num);
  for(int i = 0; i < num;i++)
    {
      printf("%d\n",yarra[i]);
    }
  
  delete [] yarra;

  
  delete serv;
  return 0;
}
#endif
