#ifndef __MUTEX_H__
#define __MUTEX_H__
#include <pthread.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct _MUTEX
{
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    int bits;
    int nowait;
}MUTEX;
void mutex_init(MUTEX *m);
void mutex_wait(MUTEX *m);
void mutex_timedwait(MUTEX *m, int usec);
void mutex_signal(MUTEX *m);
void mutex_destroy(MUTEX *m);
#define MUTEX_INIT(m) mutex_init(&(m))
#define MUTEX_LOCK(m) pthread_mutex_lock(&((m).mutex))
#define MUTEX_UNLOCK(m) pthread_mutex_unlock(&((m).mutex))
#define MUTEX_WAIT(m) mutex_wait(&(m))
#define MUTEX_TIMEDWAIT(m, usec) mutex_timedwait(&(m), usec)
#define MUTEX_COND_WAIT(m) pthread_cond_wait(&((m).cond), &((m).mutex))
#define MUTEX_SIGNAL(m) mutex_signal(&(m))
#define MUTEX_DESTROY(m) mutex_destroy(&(m))
#ifdef __cplusplus
 }
#endif
#endif
