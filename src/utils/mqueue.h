#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include "mutex.h"
#ifndef _MQUEUE64_H
#define _MQUEUE64_H
#ifdef __cplusplus
extern "C" {
#endif
typedef struct _MQODE
{
    //void *ptr;
    unsigned int data;
    unsigned int next;
}MQNODE;
typedef struct _MQROOT
{
    int status;
    int total;
    int first;
    int last;
}MQROOT;
#define MQ_INCRE_NUM       1000000
#define MQ_NODE_MAX        100000000
#define MQ_ROOT_MAX        5000000
typedef struct _MQSTATE
{
    int     qtotal;
    int     qleft;
    int     nroots;
    MQROOT  roots[MQ_ROOT_MAX];
}MQSTATE;
typedef struct _MQUEUE
{
    int      fd;
    int      bits;
    off_t    end;
    off_t    size;
    off_t    old;
    void     *map;
    MUTEX    *mutex;
    MQSTATE *state;
    MQNODE  *nodes;
}MQUEUE;
MQUEUE *mqueue_init(char *qfile);
int mqueue_new(MQUEUE *mmq);
int mqueue_total(MQUEUE *mmq, int rootid);
int mqueue_close(MQUEUE *mmq, int rootid);
int mqueue_push(MQUEUE *mmq, int rootid, int data);
int mqueue_pop(MQUEUE *mmq, int rootid, int *data);
int mqueue_head(MQUEUE *mmq, int rootid, int  *data);
void mqueue_clean(MQUEUE *mmq);
#define MQ(x) ((MQUEUE*)x)
#ifdef __cplusplus
     }
#endif
#endif
