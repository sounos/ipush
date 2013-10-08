#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <errno.h>
#include "mutex.h"
#include "mqueue.h"
#include "xmm.h"
MQUEUE *mqueue_init()
{
    MQUEUE *mmq = NULL;

    if((mmq = (MQUEUE *)xmm_mnew(sizeof(MQUEUE))))
    {
        MUTEX_INIT(mmq->mutex);
        mmq->size = sizeof(MQNODE) * MQ_NODE_MAX + sizeof(MQSTATE);
        if((mmq->map = mmap(NULL, mmq->size, PROT_READ|PROT_WRITE, MAP_ANON|MAP_SHARED,
                        -1, 0)) == NULL || mmq->map == (void *)-1)
        {
            fprintf(stderr, "mmap failed, %s\n", strerror(errno));
            _exit(-1);
        }
        mmq->state = (MQSTATE *)mmq->map;
        memset(mmq->state, 0, sizeof(MQSTATE));
        mmq->end = sizeof(MQSTATE);
        mmq->nodes = (MQNODE *)((char *)mmq->map + sizeof(MQSTATE));
    }
    return mmq;
}

/* to qleft */
int mqueue_incre(MQUEUE *mmq)
{
    int x = 0;

    if(mmq)
    {
        //fprintf(stdout, "%s::%d end:%lld qleft:%d qtotal:%d\n", __FILE__, __LINE__, (long long int)mmq->end, mmq->state->qleft, mmq->state->qtotal);
        x = (mmq->end - sizeof(MQSTATE)) / sizeof(MQNODE); 
        mmq->end += sizeof(MQNODE) * MQ_INCRE_NUM;
        mmq->state->qtotal += MQ_INCRE_NUM;
        if(x == 0) ++x;
        while(x < mmq->state->qtotal)
        {
            mmq->nodes[x].data = 0;
            mmq->nodes[x].next = mmq->state->qleft;
            mmq->state->qleft = x;
            ++x;
        }
    }
    return 0;
}

/* new queue */
int mqueue_new(MQUEUE *mmq)
{
    int rootid = -1, i = 0;

    if(mmq)
    {
        MUTEX_LOCK(mmq->mutex);
        if(mmq->state && mmq->map && mmq->state->nroots < MQ_ROOT_MAX)
        {
            i = 1;
            while(mmq->state->roots[i].status && i < MQ_ROOT_MAX) ++i;
            if(i < MQ_ROOT_MAX && mmq->state->roots[i].status == 0)
            {
                mmq->state->roots[i].status = 1;
                mmq->state->nroots++;
                mmq->state->roots[i].total = 0;
                mmq->state->roots[i].first = mmq->state->roots[i].last = 0;
                rootid = i;
            }
        }
        MUTEX_UNLOCK(mmq->mutex);
    }
    return rootid;
}

/* total */
int mqueue_total(MQUEUE *mmq, int rootid)
{
    int ret = 0;

    if(mmq)
    {
        if(mmq->state && mmq->map && rootid < MQ_ROOT_MAX)
        {
            ret = mmq->state->roots[rootid].total;
        }
    }
    return ret;
}

/* close queue */
int mqueue_close(MQUEUE *mmq, int rootid)
{
    int i = 0;

    if(mmq)
    {
        MUTEX_LOCK(mmq->mutex);
        if(mmq->state && mmq->map && rootid > 0 && rootid < MQ_ROOT_MAX)
        {
            if((i = mmq->state->roots[rootid].last) > 0)
            {
                mmq->nodes[i].next = mmq->state->qleft;
                mmq->state->qleft = mmq->state->roots[rootid].first;
            }
            memset(&(mmq->state->roots[rootid]), 0, sizeof(MQROOT));
            mmq->state->nroots--;
        }
        MUTEX_UNLOCK(mmq->mutex);
    }
    return rootid;
}

/* push */
int mqueue_push(MQUEUE *mmq, int rootid, int data)
{
    int id = -1, x = 0;

    if(mmq && rootid > 0 && rootid < MQ_ROOT_MAX)
    {
        MUTEX_LOCK(mmq->mutex);
        if(mmq->state && mmq->state->roots[rootid].status > 0)
        {
            if(mmq->state->qleft == 0) mqueue_incre(mmq);
            if((id = mmq->state->qleft) > 0 && mmq->nodes) 
            {
                mmq->state->qleft = mmq->nodes[id].next;
                if(mmq->state->roots[rootid].total == 0)  
                {
                    mmq->state->roots[rootid].first = mmq->state->roots[rootid].last = id;
                }
                else
                {
                    x = mmq->state->roots[rootid].last;
                    mmq->nodes[x].next = id;
                    mmq->state->roots[rootid].last = id;
                }
                mmq->state->roots[rootid].total++;
                mmq->nodes[id].next = 0;
                mmq->nodes[id].data = data;
            }
        }
        MUTEX_UNLOCK(mmq->mutex);
    }
    return id;
}
/* head */
int mqueue_head(MQUEUE *mmq, int rootid, int *data)
{
    int id = -1;

    if(mmq && rootid > 0 && rootid < MQ_ROOT_MAX)
    {
        MUTEX_LOCK(mmq->mutex);
        if(mmq->state && mmq->nodes && mmq->state->roots[rootid].status > 0 
                && mmq->state->roots[rootid].total > 0 
                && (id = mmq->state->roots[rootid].first) > 0)
        {
            if(data) *data = mmq->nodes[id].data;    
        }
        MUTEX_UNLOCK(mmq->mutex);
    }
    return id;
}

/* pop */
int mqueue_pop(MQUEUE *mmq, int rootid, int *data)
{
    int id = -1;

    if(mmq && rootid > 0 && rootid < MQ_ROOT_MAX)
    {
        MUTEX_LOCK(mmq->mutex);
        if(mmq->state && mmq->nodes && mmq->state->roots[rootid].status > 0 
                && mmq->state->roots[rootid].total > 0 
                && (id = mmq->state->roots[rootid].first) > 0)
        {
            if(data) *data = mmq->nodes[id].data;    
            mmq->state->roots[rootid].first = mmq->nodes[id].next;
            mmq->state->roots[rootid].total--;
            if(mmq->state->roots[rootid].total == 0)
                mmq->state->roots[rootid].last = 0;
            mmq->nodes[id].next = mmq->state->qleft;
            mmq->state->qleft = id;
        }
        MUTEX_UNLOCK(mmq->mutex);
    }
    return id;
}

/* clean */
void mqueue_clean(MQUEUE *mmq)
{
    if(mmq)
    {
        MUTEX_DESTROY(mmq->mutex);
        if(mmq->map) munmap(mmq->map, mmq->size);
        xmm_free(mmq, sizeof(MQUEUE));
    }
    return ;
}

#ifdef _DEBUG_MQUEUE
int main()
{
    int qroots[64], i = 0, j = 0, id = 0,k = 0;
    MQUEUE *mmq = NULL;

    if((mmq = mqueue_init("/tmp/mmq")))
    {
        //push
        for(i = 0; i < 64; i++)
        {
            if((qroots[i] = mqueue_new(mmq)))
            {
                mqueue_push(mmq, qroots[i], i);
                for(j = 0; j < 500000; j++)
                {
                    mqueue_push(mmq, qroots[i], j);
                }
            }
        }
        //pop
        for(i = 0; i < 64; i++)
        {
            if((id = mqueue_head(mmq, qroots[i], &k)) > 0)
            {
                fprintf(stdout, "%d:", k);
                j = 0;
                while(mqueue_pop(mmq, qroots[i], &k) > 0)
                {
                    fprintf(stdout, "%d|", k);
                    ++j;
                }
                fprintf(stdout, "[%d]\r\n", j);
            }
        }
        mqueue_clean(mmq);
    }
}
//gcc -o mmq mqueue.c xmm.c -D_DEBUG_MQUEUE && ./mmq
#endif
