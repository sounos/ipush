#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>
#include <string.h>
#include <errno.h>
#include "mutex.h"
void mutex_init(MUTEX *m)
{
    pthread_mutexattr_t attr;
    pthread_condattr_t cattr;
    if(m)
    {
        pthread_mutexattr_init(&attr);                                         
        pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
        pthread_mutex_init(&(m->mutex), &attr);
        pthread_condattr_init(&cattr);                                          
        pthread_condattr_setpshared(&cattr, PTHREAD_PROCESS_SHARED);
        pthread_cond_init(&(m->cond), &cattr);         
    }
    return ;
}


void mutex_wait(MUTEX *m)
{                                                                                 
    if(m)
    {
        pthread_mutex_lock(&(m->mutex));                                                  
        if(m->nowait == 0)                                                               
            pthread_cond_wait(&(m->cond), &(m->mutex));                                    
        m->nowait = 0;                                                                   
        pthread_mutex_unlock(&(m->mutex));                                                
    }
    return ;
}

void mutex_timedwait(MUTEX *m, int usec)
{
    struct timespec ts;
    struct timeval now;

    if(m)
    {
        gettimeofday(&now, NULL);
        ts.tv_sec = now.tv_sec + ((now.tv_usec + usec) / 1000000);
        ts.tv_nsec = ((now.tv_usec + usec) % 1000000) * 1000;
        pthread_mutex_lock(&(m->mutex));                                                  
        if(m->nowait == 0)                                                               
            pthread_cond_timedwait(&(m->cond), &(m->mutex), &ts);                                    
        m->nowait = 0;                                                                   
        pthread_mutex_unlock(&(m->mutex));                                                
    }
    return ;
}

void mutex_signal(MUTEX *m)
{                                                                                 
    if(m)
    {
        pthread_mutex_lock(&(m->mutex));                                                  
        m->nowait = 1;                                                                   
        pthread_cond_signal(&(m->cond));
        pthread_mutex_unlock(&(m->mutex));                                                
    }
    return ;
}
void mutex_destroy(MUTEX *m)
{
    if(m)
    {
        pthread_mutex_destroy(&(m->mutex)); 						                        
        pthread_cond_destroy(&(m->cond)); 							                    
    }
    return ;
}

#ifdef __DEBUG__MUTEX
#include <unistd.h>
#include <stdio.h>
#include <sys/mman.h>
typedef struct _MM
{
    MUTEX m;
    int x;
    int n;
}MM;
void run(void *arg)
{
    //MM *mm = (MM *)arg;
    pthread_mutex_t *mutex = NULL;

    if((mutex = (pthread_mutex_t *)arg))
    {
        pthread_mutex_lock(mutex);
        fprintf(stdout, "start-pid:%d\n", getpid());
        sleep(20);
        fprintf(stdout, "over-pid:%d\n", getpid());
        pthread_mutex_unlock(mutex);
    }
    return ;
}
int main()
{
    //MM mm = {0};
    MM *mm = (MM *)mmap(NULL, sizeof(MM), PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANON, -1, 0);
    mutex_init(&(mm->m));
    //
    //pthread_mutex_t *mutex = (pthread_mutex_t *)mmap(NULL, sizeof(pthread_mutex_t), PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANON, -1, 0);
    //pthread_mutexattr_t attr;
    //pthread_mutexattr_init(&attr);
    //pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    //pthread_mutex_init(mutex, &attr);

    //pthread_mutex_t mutex = {0};
    //pthread_mutex_init(&mutex, NULL);

    /*
    pthread_t threadid = 0;
    if(pthread_create(&threadid, NULL, (void *)(&run), (void *)&(mm->m.mutex)) == 0)
    {
        sleep(5);
    }
    fprintf(stdout, "selfid:%p child:%p\n", pthread_self(), threadid);
    pthread_mutex_lock(&(mm->m.mutex));
    fprintf(stdout, "pid:%p child:%p\n", pthread_self(), threadid);
    pthread_mutex_unlock(&(mm->m.mutex));
    */
    //while(1) sleep(1000);
    pid_t pid = fork();
    
    if(pid == 0)
    {
        MUTEX_LOCK(mm->m);
        fprintf(stdout, "child-pid:%d mm->m:%p\n", getpid(), &(mm->m.mutex));
        sleep(10);
        //run(&(mm->m));
        MUTEX_UNLOCK(mm->m);
    }
    else
    {
        MUTEX_LOCK(mm->m);
        //pthread_mutex_lock(&(mm->m.mutex));
        fprintf(stdout, "self-pid:%d mm->m:%p\n", getpid(), &(mm->m.mutex));
        sleep(10);
        //pthread_mutex_unlock(&(mm->m.mutex));
        MUTEX_UNLOCK(mm->m);
    }
}
#endif
