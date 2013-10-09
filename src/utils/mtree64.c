#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <errno.h>
#include "mtree64.h"
#include "mutex.h"
#include "xmm.h"
#define MT(px) ((MTREE64 *)px)
#define MT_COLOR_BLACK  0
#define MT_COLOR_RED    1
#define MT_MIN_MAX(x, key, xid)                                                \
do                                                                              \
{                                                                               \
    if(MT(x) && MT(x)->state)                                                 \
    {                                                                           \
        if(MT(x)->state->count == 0)                                           \
        {                                                                       \
            MT(x)->state->nmin = MT(x)->state->nmax = xid;                    \
            MT(x)->state->kmin = MT(x)->state->kmax = key;                    \
        }                                                                       \
        else if(key > MT(x)->state->kmax)                                      \
        {                                                                       \
            MT(x)->state->nmax = xid;                                          \
            MT(x)->state->kmax = key;                                          \
        }                                                                       \
        else if(key < MT(x)->state->kmin)                                      \
        {                                                                       \
            MT(x)->state->nmin = xid;                                          \
            MT(x)->state->kmin = key;                                          \
        }                                                                       \
    }                                                                           \
}while(0)
#define MT_MUNMAP(x)                                                           \
do                                                                              \
{                                                                               \
    if(x && MT(x)->size > 0)                                                   \
    {                                                                           \
        if(MT(x)->start && MT(x)->start != (void *)-1)                        \
        {                                                                       \
            munmap(MT(x)->start, MT(x)->size);                                \
            MT(x)->start = NULL;                                               \
            MT(x)->state = NULL;                                               \
            MT(x)->map = NULL;                                                 \
        }                                                                       \
    }                                                                           \
}while(0)

#define MT_MAP(x)                                                             \
do                                                                              \
{                                                                               \
    if(x)                                                                       \
    {                                                                           \
        if((MT(x)->start = (char *)mmap(NULL,MT(x)->size,PROT_READ|PROT_WRITE,\
            MAP_ANON|MAP_SHARED, -1, 0)) != (void *)-1)              \
        {                                                                       \
            MT(x)->state = (MTSTATE64 *)MT(x)->start;                           \
            MT(x)->map = (MTNODE64 *)(MT(x)->start + sizeof(MTSTATE64));          \
        }                                                                       \
    }                                                                           \
}while(0)

#define MT_INCRE(x)                                                            \
do                                                                              \
{                                                                               \
    if(x &&  MT(x)->end <  MT(x)->size)                                       \
    {                                                                           \
        MT(x)->old = MT(x)->end;                                              \
        MT(x)->end += (off_t)MTREE64_INCRE_NUM * (off_t)sizeof(MTNODE64);       \
        if(MT(x)->old == sizeof(MTSTATE64))                                  \
        {                                                                   \
            memset(MT(x)->state, 0, sizeof(MTSTATE64));                      \
            MT(x)->state->left += MTREE64_INCRE_NUM - 1;                  \
        }                                                                   \
        else                                                                \
        {                                                                   \
            MT(x)->state->left += MTREE64_INCRE_NUM;                      \
        }                                                                   \
        MT(x)->state->total += MTREE64_INCRE_NUM;                         \
        memset(MT(x)->start + MT(x)->old, 0, MT(x)->end - MT(x)->old);  \
    }                                                                           \
}while(0)
#define MT_ROTATE_LEFT(x, prootid, oid, rid, lid, ppid)                        \
do                                                                              \
{                                                                               \
    if(x && (rid = MT(x)->map[oid].right) > 0)                                 \
    {                                                                           \
        if((lid = MT(x)->map[oid].right = MT(x)->map[rid].left) > 0)          \
        {                                                                       \
            MT(x)->map[lid].parent = oid;                                      \
        }                                                                       \
        if((ppid = MT(x)->map[rid].parent = MT(x)->map[oid].parent) > 0)      \
        {                                                                       \
            if(MT(x)->map[ppid].left == oid)                                   \
                MT(x)->map[ppid].left = rid;                                   \
            else                                                                \
                MT(x)->map[ppid].right = rid;                                  \
        }else *prootid = rid;                                                   \
        MT(x)->map[rid].left = oid;                                            \
        MT(x)->map[oid].parent = rid;                                          \
    }                                                                           \
}while(0)

#define MT_ROTATE_RIGHT(x, prootid, oid, lid, rid, ppid)                       \
do                                                                              \
{                                                                               \
    if(x && (lid = MT(x)->map[oid].left) > 0)                                  \
    {                                                                           \
        if((rid = MT(x)->map[oid].left = MT(x)->map[lid].right) > 0)          \
        {                                                                       \
            MT(x)->map[rid].parent = oid;                                      \
        }                                                                       \
        if((ppid = MT(x)->map[lid].parent = MT(x)->map[oid].parent) > 0)      \
        {                                                                       \
            if(MT(x)->map[ppid].left == oid)                                   \
                MT(x)->map[ppid].left = lid;                                   \
            else                                                                \
                MT(x)->map[ppid].right = lid;                                  \
        }                                                                       \
        else *prootid = lid;                                                    \
        MT(x)->map[lid].right = oid;                                           \
        MT(x)->map[oid].parent = lid;                                          \
    }                                                                           \
}while(0)

#define MT_INSERT_COLOR(x, prootid, oid, lid, rid, uid, pid, gpid, ppid)       \
do                                                                              \
{                                                                               \
    while((pid = MT(x)->map[oid].parent)> 0                                    \
            && MT(x)->map[pid].color == MT_COLOR_RED)                         \
    {                                                                           \
        gpid = MT(x)->map[pid].parent;                                         \
        if(pid == MT(x)->map[gpid].left)                                       \
        {                                                                       \
            uid = MT(x)->map[gpid].right;                                      \
            if(uid > 0 && MT(x)->map[uid].color == MT_COLOR_RED)              \
            {                                                                   \
                MT(x)->map[uid].color = MT_COLOR_BLACK;                       \
                MT(x)->map[pid].color = MT_COLOR_BLACK;                       \
                MT(x)->map[gpid].color = MT_COLOR_RED;                        \
                oid = gpid;                                                     \
                continue;                                                       \
            }                                                                   \
            if(MT(x)->map[pid].right == oid)                                   \
            {                                                                   \
                MT_ROTATE_LEFT(x, prootid, pid, rid, lid, ppid);               \
                uid = pid; pid = oid; oid = uid;                                \
            }                                                                   \
            MT(x)->map[pid].color = MT_COLOR_BLACK;                           \
            MT(x)->map[gpid].color = MT_COLOR_RED;                            \
            MT_ROTATE_RIGHT(x, prootid, gpid, lid, rid, ppid);                 \
        }                                                                       \
        else                                                                    \
        {                                                                       \
            uid = MT(x)->map[gpid].left;                                       \
            if(uid > 0 && MT(x)->map[uid].color == MT_COLOR_RED)              \
            {                                                                   \
                MT(x)->map[uid].color = MT_COLOR_BLACK;                       \
                MT(x)->map[pid].color = MT_COLOR_BLACK;                       \
                MT(x)->map[gpid].color = MT_COLOR_RED;                        \
                oid = gpid;                                                     \
                continue;                                                       \
            }                                                                   \
            if(MT(x)->map[pid].left == oid)                                    \
            {                                                                   \
                MT_ROTATE_RIGHT(x, prootid, pid, lid, rid, ppid);              \
                uid = pid; pid = oid; oid = uid;                                \
            }                                                                   \
            MT(x)->map[pid].color = MT_COLOR_BLACK;                           \
            MT(x)->map[gpid].color = MT_COLOR_RED;                            \
            MT_ROTATE_LEFT(x, prootid, gpid, rid, lid, ppid);                  \
        }                                                                       \
    }                                                                           \
    if(*prootid > 0)MT(x)->map[*prootid].color = MT_COLOR_BLACK;              \
}while(0)

#define MT_REMOVE_COLOR(x, prootid, oid, xpid, lid, rid, uid, ppid)            \
do                                                                              \
{                                                                               \
    while((oid == 0 || MT(x)->map[oid].color == MT_COLOR_BLACK)               \
            && oid != *prootid)                                                 \
    {                                                                           \
        if(MT(x)->map[xpid].left == oid)                                       \
        {                                                                       \
            uid = MT(x)->map[xpid].right;                                      \
            if(MT(x)->map[uid].color == MT_COLOR_RED)                         \
            {                                                                   \
                MT(x)->map[uid].color = MT_COLOR_BLACK;                       \
                MT(x)->map[xpid].color = MT_COLOR_RED;                        \
                MT_ROTATE_LEFT(x, prootid, xpid, rid, lid, ppid);              \
                uid = MT(x)->map[xpid].right;                                  \
            }                                                                   \
            lid = MT(x)->map[uid].left;                                        \
            rid = MT(x)->map[uid].right;                                       \
            if((lid == 0 || MT(x)->map[lid].color == MT_COLOR_BLACK)          \
                && (rid == 0 || MT(x)->map[rid].color == MT_COLOR_BLACK))     \
            {                                                                   \
                MT(x)->map[uid].color = MT_COLOR_RED;                         \
                oid = xpid;                                                      \
                xpid = MT(x)->map[oid].parent;                                  \
            }                                                                   \
            else                                                                \
            {                                                                   \
                rid = MT(x)->map[uid].right;                                   \
                lid = MT(x)->map[uid].left;                                    \
                if(rid == 0 || MT(x)->map[rid].color == MT_COLOR_BLACK)       \
                {                                                               \
                    if(lid > 0)MT(x)->map[lid].color = MT_COLOR_BLACK;        \
                    MT(x)->map[uid].color = MT_COLOR_RED;                     \
                    MT_ROTATE_RIGHT(x, prootid, uid, lid, rid, ppid);          \
                    uid = MT(x)->map[xpid].right;                              \
                }                                                               \
                MT(x)->map[uid].color = MT(x)->map[xpid].color;               \
                MT(x)->map[xpid].color = MT_COLOR_BLACK;                      \
                if((rid = MT(x)->map[uid].right) > 0)                          \
                    MT(x)->map[rid].color = MT_COLOR_BLACK;                   \
                MT_ROTATE_LEFT(x, prootid, xpid, rid, lid, ppid);              \
                oid = *prootid;                                                 \
                break;                                                          \
            }                                                                   \
        }                                                                       \
        else                                                                    \
        {                                                                       \
            uid = MT(x)->map[xpid].left;                                       \
            if(MT(x)->map[uid].color == MT_COLOR_RED)                         \
            {                                                                   \
                MT(x)->map[uid].color = MT_COLOR_BLACK;                       \
                MT(x)->map[xpid].color = MT_COLOR_RED;                        \
                MT_ROTATE_RIGHT(x, prootid, xpid, lid, rid, ppid);             \
                uid = MT(x)->map[xpid].left;                                   \
            }                                                                   \
            lid = MT(x)->map[uid].left;                                        \
            rid = MT(x)->map[uid].right;                                       \
            if((lid == 0 || MT(x)->map[lid].color == MT_COLOR_BLACK)          \
                && (rid == 0 || MT(x)->map[rid].color == MT_COLOR_BLACK))     \
            {                                                                   \
                MT(x)->map[uid].color = MT_COLOR_RED;                         \
                oid = xpid;                                                     \
                xpid = MT(x)->map[oid].parent;                                 \
            }                                                                   \
            else                                                                \
            {                                                                   \
                rid = MT(x)->map[uid].right;                                   \
                lid = MT(x)->map[uid].left;                                    \
                if(lid == 0 || MT(x)->map[lid].color == MT_COLOR_BLACK)       \
                {                                                               \
                    if(rid > 0)MT(x)->map[rid].color = MT_COLOR_BLACK;        \
                    MT(x)->map[uid].color = MT_COLOR_RED;                     \
                    MT_ROTATE_LEFT(x, prootid, uid, rid, lid, ppid);           \
                    uid = MT(x)->map[xpid].left;                               \
                }                                                               \
                MT(x)->map[uid].color = MT(x)->map[xpid].color;               \
                MT(x)->map[xpid].color = MT_COLOR_BLACK;                      \
                if((lid = MT(x)->map[uid].left) > 0)                           \
                    MT(x)->map[lid].color = MT_COLOR_BLACK;                   \
                MT_ROTATE_RIGHT(x, prootid, xpid, lid, rid, ppid);             \
                oid = *prootid;                                                 \
                break;                                                          \
            }                                                                   \
        }                                                                       \
    }                                                                           \
    if(oid > 0) MT(x)->map[oid].color = MT_COLOR_BLACK;                       \
}while(0)
void mtree64_mutex_lock(void *x, int id)
{
    if(x)
    {
#ifdef HAVE_PTHREAD
        pthread_mutex_lock(&(MT(x)->mutexs[id%MTREE64_MUTEX_MAX]));
#endif
    }
    return ;
}
void mtree64_mutex_unlock(void *x, int id)
{
    if(x)
    {
#ifdef HAVE_PTHREAD
        pthread_mutex_unlock(&(MT(x)->mutexs[id%MTREE64_MUTEX_MAX]));
#endif
    }
    return ;
}

/* init mtree */
void *mtree64_init()
{
    int i = 0;
    void *x = NULL;

    if((x = (MTREE64 *)xmm_mnew(sizeof(MTREE64))))
    {
        MUTEX_INIT(MT(x)->mutex);
        MT(x)->size = (off_t)sizeof(MTSTATE64) + (off_t)sizeof(MTNODE64) * (off_t)MTREE64_NODES_MAX;
        //mmap
        MT_MAP(x);
        MT(x)->end = (off_t)sizeof(MTSTATE64);
        MT_INCRE(x);
        /* initialize mutexs  */
#ifdef HAVE_PTHREAD
        for(i = 0; i < MTREE64_MUTEX_MAX; i++)
        {
            pthread_mutex_init(&(MT(x)->mutexs[i]), NULL);
        }
#endif
    }
    return x;
}

/* insert new root */
int mtree64_new_tree(void *x)
{
    int id = 0, i = 0;
    if(x)
    {
        MUTEX_LOCK(MT(x)->mutex);
        if(MT(x)->state->nroots == 0) MT(x)->state->nroots = 1;
        if(MT(x)->state && MT(x)->state->nroots < MTREE64_ROOT_MAX)
        {
            for(i = 1; i < MTREE64_ROOT_MAX; i++)
            {
                if(MT(x)->state->roots[i].status == 0)
                {
                    MT(x)->state->roots[i].status = 1;
                    MT(x)->state->nroots++;
                    id = i;
                    break;
                }
            }
        }
        MUTEX_UNLOCK(MT(x)->mutex);
    }
    return id;
}

/* total */
unsigned int mtree64_total(void *x, int rootid)
{
    unsigned int total = 0;

    if(x && rootid > 0)
    {
        MUTEX_LOCK(MT(x)->mutex);
        if(MT(x)->state && MT(x)->map && rootid < MTREE64_ROOT_MAX)
        {
            total =  MT(x)->state->roots[rootid].total;
        }
        MUTEX_UNLOCK(MT(x)->mutex);
    }
    return total;
}

//add nodeid to qleft
void mtree64_qleft(void *x, int tnodeid)
{
    int z = 0;
    if(x)
    {
        MUTEX_LOCK(MT(x)->mutex);
        memset(&(MT(x)->map[tnodeid]), 0, sizeof(MTNODE64));
        if(MT(x)->state->qleft == 0)
        {
            MT(x)->state->qfirst = MT(x)->state->qlast = tnodeid;
        }
        else
        {
            z = MT(x)->state->qlast;
            MT(x)->map[z].parent = tnodeid;
            MT(x)->state->qlast = tnodeid;
        }
        MT(x)->state->qleft++;
        MT(x)->state->left++;
        MUTEX_UNLOCK(MT(x)->mutex);
    }
    return ;
}
//new node
unsigned int mtree64_new_node(void *x, int rootid, int nodeid, int64_t key, int data)
{
    unsigned int id = 0;

    if(x)
    {
        MUTEX_LOCK(MT(x)->mutex);
        //fprintf(stdout, "%s::%d %d start:%p state:%p map:%p current:%d left:%d total:%d qleft:%d qfirst:%d qlast:%d\n", __FILE__, __LINE__, id, MT(x)->start, MT(x)->state, MT(x)->map, MT(x)->state->current, MT(x)->state->left, MT(x)->state->total, MT(x)->state->qleft, MT(x)->state->qfirst, MT(x)->state->qlast);
        if(MT(x)->state->left == 0)
        {
            MT_INCRE(x);
        }
        if(MT(x)->state->qleft > 0)
        {
            id = MT(x)->state->qfirst;
            MT(x)->state->qfirst = MT(x)->map[id].parent;
            MT(x)->state->qleft--;
        }
        else
        {
            id = ++(MT(x)->state->current);
        }
        //fprintf(stdout, "%s::%d %d start:%p state:%p map:%p current:%d left:%d total:%d qleft:%d qfirst:%d qlast:%d\n", __FILE__, __LINE__, id, MT(x)->start, MT(x)->state, MT(x)->map, MT(x)->state->current, MT(x)->state->left, MT(x)->state->total, MT(x)->state->qleft, MT(x)->state->qfirst, MT(x)->state->qlast);
        MT(x)->state->left--;
        //memset(&(MT(x)->map[id]), 0, sizeof(MTNODE64));
        MT(x)->map[id].parent = nodeid;
        MT(x)->map[id].key = key;
        MT(x)->map[id].data = data;
        MT_MIN_MAX(x, id, key);
        if(nodeid > 0)
        {
            if(key > MT(x)->map[nodeid].key) 
                MT(x)->map[nodeid].right = id;
            else
                MT(x)->map[nodeid].left = id;
        }
        MT(x)->state->roots[rootid].total++;
        MUTEX_UNLOCK(MT(x)->mutex);
    }
    return id;
}

/* insert new node */
unsigned int mtree64_insert(void *x, int rootid, int64_t key, int data, int *old)
{
    unsigned int id = 0, nodeid = 0, rid = 0, lid = 0, uid = 0, pid = 0, 
        gpid = 0, ppid = 0, *prootid = NULL;
    MTNODE64 *node = NULL;

    if(x && rootid > 0)
    {
        mtree64_mutex_lock(x, rootid);
        if(MT(x)->state && MT(x)->map && rootid < MTREE64_ROOT_MAX
                && MT(x)->state->roots[rootid].status > 0)
        {
            nodeid = MT(x)->state->roots[rootid].rootid;
            while(nodeid > 0 && nodeid < MT(x)->state->total)
            {
                node = &(MT(x)->map[nodeid]);
                if(key == node->key)
                {
                    id = nodeid;
                    if(old) *old = node->data;
                    node->data = data;
                    goto end;
                }
                else if(key > node->key)
                {
                    if(node->right == 0) break;
                    nodeid = node->right;
                }
                else 
                {
                    if(node->left == 0) break;
                    nodeid = node->left;
                }
            }
            //new node
            if(id == 0) id = mtree64_new_node(x, rootid, nodeid, key, data);
        }
        if((nodeid = id) > 0)
        {
            if(MT(x)->state->roots[rootid].rootid > 0)
            {
                prootid = &(MT(x)->state->roots[rootid].rootid);
                MT(x)->map[nodeid].color = MT_COLOR_RED;
                MT_INSERT_COLOR(x, prootid, nodeid, lid, rid, uid, pid, gpid, ppid);
            }
            else
            {
                MT(x)->state->roots[rootid].rootid = nodeid;
            }
        }
end:
        mtree64_mutex_unlock(x, rootid);
    }
    return id;
}


/* try insert  node */
unsigned int mtree64_try_insert(void *x, int rootid, int64_t key, int data, int *old)
{
    unsigned int id = 0, nodeid = 0, rid = 0, lid = 0, uid = 0, pid = 0, 
        gpid = 0, ppid = 0, *prootid = NULL;
    MTNODE64 *node = NULL;

    if(x && rootid > 0)
    {
        mtree64_mutex_lock(x, rootid);
        if(MT(x)->state && MT(x)->map && rootid < MTREE64_ROOT_MAX
                && MT(x)->state->roots[rootid].status > 0)
        {
            nodeid = MT(x)->state->roots[rootid].rootid;
            while(nodeid > 0 && nodeid < MT(x)->state->total)
            {
                node = &(MT(x)->map[nodeid]);
                if(key == node->key)
                {
                    id = nodeid;
                    if(old) *old = node->data;
                    //fprintf(stdout, "%s::%d id:%d key:%lld old[%lld]->data:%d\n", __FILE__, __LINE__, nodeid, (long long)key, (long long)node->key, node->data);
                    goto end;
                }
                else if(key > node->key)
                {
                    if(node->right == 0) break;
                    nodeid = node->right;
                }
                else 
                {
                    if(node->left == 0) break;
                    nodeid = node->left;
                }
            }
        }
        if(id == 0) id = mtree64_new_node(x, rootid, nodeid, key, data);
        if((nodeid = id) > 0)
        {
            if(MT(x)->state->roots[rootid].rootid > 0)
            {
                prootid = &(MT(x)->state->roots[rootid].rootid);
                MT(x)->map[nodeid].color = MT_COLOR_RED;
                MT_INSERT_COLOR(x, prootid, nodeid, lid, rid, uid, pid, gpid, ppid);
            }
            else
            {
                MT(x)->state->roots[rootid].rootid = nodeid;
            }
        }
end:
        mtree64_mutex_unlock(x, rootid);
    }
    return id;
}


/* get node key/data */
unsigned int mtree64_get(void *x, unsigned int tnodeid, int64_t *key, int *data)
{
    unsigned int id = 0;

    if(x && tnodeid > 0)
    {
        MUTEX_LOCK(MT(x)->mutex);
        if(MT(x)->map && MT(x)->state && tnodeid <  MT(x)->state->total)
        {
            if(key) *key = MT(x)->map[tnodeid].key;
            if(data) *data = MT(x)->map[tnodeid].data;
            id = tnodeid;
        }
        MUTEX_UNLOCK(MT(x)->mutex);
    }
    return id;
}

/* find key/data */
unsigned int mtree64_find(void *x, int rootid, int64_t key, int *data)
{
    unsigned int id = 0;

    if(x && rootid > 0)
    {
        mtree64_mutex_lock(x, rootid);
        if(MT(x)->map && MT(x)->state && rootid < MTREE64_ROOT_MAX
                && MT(x)->state->roots[rootid].status > 0)
        {
            //fprintf(stdout, "%s::%d rootid:%d key:%d data:%d total:%d\n", __FILE__, __LINE__, rootid, key, *data, MT(x)->state->total);
            id = MT(x)->state->roots[rootid].rootid;
            while(id > 0 && id < MT(x)->state->total)
            {
                if(key == MT(x)->map[id].key)
                {
                    if(data) *data = MT(x)->map[id].data;
                    break;
                }
                else if(key > MT(x)->map[id].key)
                {
                    id = MT(x)->map[id].right;
                }
                else
                {
                    id = MT(x)->map[id].left;
                }
            }
        }
        //fprintf(stdout, "%s::%d rootid:%d key:%d data:%d\n", __FILE__, __LINE__, rootid, key, *data);
        mtree64_mutex_unlock(x, rootid);
    }
    return id;
}

/* get tree->min key/data */
unsigned int mtree64_min(void *x, int rootid, int64_t *key, int *data)
{
    unsigned int id = 0;

    if(x && rootid > 0)
    {
        mtree64_mutex_lock(x, rootid);
        if(MT(x)->map && MT(x)->state && rootid <  MTREE64_ROOT_MAX
                && MT(x)->state->roots[rootid].status > 0)
        {
            id = MT(x)->state->roots[rootid].rootid;
            while(MT(x)->map[id].left > 0)
            {
                id = MT(x)->map[id].left;
            }
            if(id > 0 && MT(x)->state->total)
            {
                if(key) *key = MT(x)->map[id].key;
                if(data) *data = MT(x)->map[id].data;
            }
        }
        mtree64_mutex_unlock(x, rootid);
    }
    return id;
}

/* get tree->max key/data */
unsigned  int mtree64_max(void *x, int rootid, int64_t *key, int *data)
{
    unsigned int id = 0, tmp = 0;

    if(x && rootid > 0)
    {
        mtree64_mutex_lock(x, rootid);
        if(MT(x)->map && MT(x)->state && rootid <  MTREE64_ROOT_MAX
                && MT(x)->state->roots[rootid].status > 0)
        {
            tmp = MT(x)->state->roots[rootid].rootid;
            do
            {
                id = tmp;
            }while(id > 0 && (tmp = MT(x)->map[id].right) > 0);
            if(id > 0 && MT(x)->state->total)
            {
                if(key) *key = MT(x)->map[id].key;
                if(data) *data = MT(x)->map[id].data;
            }
        }
        mtree64_mutex_unlock(x, rootid);
    }
    return id;
}

/* get next node key/data */
unsigned int mtree64_next(void *x, int rootid, unsigned int tnodeid, int64_t *key, int *data)
{
    unsigned int id = 0, parentid = 0;

    if(x && tnodeid > 0 && rootid > 0)
    {
        mtree64_mutex_lock(x, rootid);
        if(MT(x)->map && MT(x)->state && tnodeid <  MT(x)->state->total)
        {
            id = tnodeid;
            if(MT(x)->map[id].right > 0)
            {
                id = MT(x)->map[id].right;
                while(MT(x)->map[id].left  > 0)
                {
                    id = MT(x)->map[id].left;
                }
                //fprintf(stdout, "%s::%d id:%d\n", __FILE__, __LINE__, id);
            }
            else
            {
                while(id > 0)
                {
                    parentid = MT(x)->map[id].parent;
                    if(MT(x)->map[id].key < MT(x)->map[parentid].key)
                    {
                        id = parentid;
                        goto end;
                    }
                    else
                    {
                        id = parentid;
                    }
                }
                //fprintf(stdout, "%s::%d id:%d\n", __FILE__, __LINE__, id);
            }
end:
            if(id > 0 && id < MT(x)->state->total)
            {
                if(key) *key = MT(x)->map[id].key;
                if(data) *data = MT(x)->map[id].data;
            }
            //fprintf(stdout, "%s::%d rootid:%d tnodeid:%d id:%d\n",__FILE__, __LINE__, rootid, tnodeid, id);
        }
        mtree64_mutex_unlock(x, rootid);
    }
    return id;
}

/* get prev node key/data */
unsigned int mtree64_prev(void *x, int rootid, unsigned int tnodeid, int64_t *key, int *data)
{
    unsigned int id = 0, parentid = 0;

    if(x && tnodeid > 0 && rootid > 0)
    {
        mtree64_mutex_lock(x, rootid);
        if(MT(x)->map && MT(x)->state && tnodeid <  MT(x)->state->total)
        {
            id = tnodeid;
            if(MT(x)->map[id].left > 0)
            {
                id = MT(x)->map[id].left;
                while(MT(x)->map[id].right  > 0)
                {
                    id = MT(x)->map[id].right;
                }
                //fprintf(stdout, "%s::%d id:%d\n", __FILE__, __LINE__, id);
            }
            else
            {
                while(id > 0)
                {
                    parentid = MT(x)->map[id].parent;
                    if(MT(x)->map[id].key > MT(x)->map[parentid].key)
                    {
                        id = parentid;
                        goto end;
                    }
                    else
                    {
                        id = parentid;
                    }
                }
                //fprintf(stdout, "%s::%d id:%d\n", __FILE__, __LINE__, id);
            }
end:
            if(id > 0 && id < MT(x)->state->total)
            {
                if(key)*key = MT(x)->map[id].key;
                if(data)*data = MT(x)->map[id].data;
            }
            //fprintf(stdout, "%s::%d rootid:%d tnodeid:%d id:%d\n",__FILE__, __LINE__, rootid, tnodeid, id);
        }
        mtree64_mutex_unlock(x, rootid);
    }
    return id;
}

/* view node */
void mtree64_view_tnode(void *x, unsigned int tnodeid, FILE *fp)
{
    if(x)
    {
        if(MT(x)->map[tnodeid].left > 0 && MT(x)->map[tnodeid].left < MT(x)->state->total)
        {
            mtree64_view_tnode(x, MT(x)->map[tnodeid].left, fp);
        }
        fprintf(fp, "[%d:%lld:%d]\n", tnodeid, (long long)MT(x)->map[tnodeid].key, MT(x)->map[tnodeid].data);
        if(MT(x)->map[tnodeid].right > 0 && MT(x)->map[tnodeid].right < MT(x)->state->total)
        {
            mtree64_view_tnode(x, MT(x)->map[tnodeid].right, fp);
        }
    }
    return ;
}

void mtree64_view_tree(void *x, int rootid, FILE *fp)
{
    if(x && rootid > 0)
    {
        mtree64_mutex_lock(x, rootid);
        if(MT(x)->map && MT(x)->state && rootid < MTREE64_ROOT_MAX)
        {
            fprintf(stdout, "%s::%d rootid:%d\n", __FILE__, __LINE__, MT(x)->state->roots[rootid].rootid);
             mtree64_view_tnode(x, MT(x)->state->roots[rootid].rootid, fp);
        }
        mtree64_mutex_unlock(x, rootid);
    }
    return ;
}

/* set data */
int mtree64_set_data(void *x, unsigned int tnodeid, int data)
{
    int old = -1;

    if(x && tnodeid > 0)
    {
        MUTEX_LOCK(MT(x)->mutex);
        if(MT(x)->map && MT(x)->state && tnodeid < MT(x)->state->total)
        {
            old = MT(x)->map[tnodeid].data;
            MT(x)->map[tnodeid].data = data;
        }
        MUTEX_UNLOCK(MT(x)->mutex);
    }
    return old;
}

/* remove node */
void mtree64_remove(void *x, int rootid, unsigned int tnodeid, int64_t *key, int *data)
{
    unsigned int id = 0, pid = 0, parent = 0, child = 0, rid = 0, lid = 0,
        uid = 0, ppid = 0, color = 0, *prootid = NULL;

    if(x && rootid > 0 && tnodeid > 0)
    {
        mtree64_mutex_lock(x, rootid);
        if(MT(x)->map && MT(x)->state && tnodeid < MT(x)->state->total)
        {
            if(key) *key = MT(x)->map[tnodeid].key;
            if(data) *data = MT(x)->map[tnodeid].data;
            id = tnodeid;
            if(MT(x)->map[tnodeid].left == 0)
            {
                child = MT(x)->map[tnodeid].right;
            }
            else if(MT(x)->map[tnodeid].right == 0)
            {
                child = MT(x)->map[tnodeid].left;
            }
            else 
            {
                id = MT(x)->map[tnodeid].right;
                while(MT(x)->map[id].left > 0)
                    id = MT(x)->map[id].left;
                parent = MT(x)->map[id].parent;
                color = MT(x)->map[id].color;
                if((child = MT(x)->map[id].right) > 0)
                    MT(x)->map[child].parent = parent;
                if((pid = parent) > 0)
                {
                    if(MT(x)->map[pid].left == id)
                        MT(x)->map[pid].left = child;
                    else
                        MT(x)->map[pid].right = child;
                }
                else
                {
                    MT(x)->state->roots[rootid].rootid = child;
                }
                if(MT(x)->map[id].parent == tnodeid) parent = id;
                MT(x)->map[id].color = MT(x)->map[tnodeid].color;
                MT(x)->map[id].parent = MT(x)->map[tnodeid].parent;
                MT(x)->map[id].left = MT(x)->map[tnodeid].left;
                MT(x)->map[id].right = MT(x)->map[tnodeid].right;
                if((pid = MT(x)->map[tnodeid].parent) > 0)
                {
                    if(MT(x)->map[pid].left == tnodeid)
                        MT(x)->map[pid].left = id;
                    else
                        MT(x)->map[pid].right = id;
                }
                else
                {
                    MT(x)->state->roots[rootid].rootid = id;
                }
                lid = MT(x)->map[tnodeid].left;
                MT(x)->map[lid].parent = id;
                if((rid = MT(x)->map[tnodeid].right) > 0)
                    MT(x)->map[rid].parent = id;
                goto color_remove;
            }
            parent =  MT(x)->map[tnodeid].parent;
            color = MT(x)->map[tnodeid].color;
            if(child > 0) 
            {
                MT(x)->map[child].parent = parent;
            }
            if((pid = parent) > 0)
            {
                if(tnodeid == MT(x)->map[pid].left) 
                    MT(x)->map[pid].left = child;
                else 
                    MT(x)->map[pid].right = child;
            }
            else 
            {
                MT(x)->state->roots[rootid].rootid = child;
            }
            //remove color set
color_remove:
            MT(x)->state->roots[rootid].total--;
            if(color == MT_COLOR_BLACK)
            {
                //fprintf(stdout, "%s::%d node:%d parent:%d left:%d right:%d key:%d data:%d\n", __FILE__, __LINE__, tnodeid, MT(x)->map[tnodeid].parent, MT(x)->map[tnodeid].left, MT(x)->map[tnodeid].right, MT(x)->map[tnodeid].key, MT(x)->map[tnodeid].data);
                prootid = &(MT(x)->state->roots[rootid].rootid);
                MT_REMOVE_COLOR(x, prootid, child, parent, lid, rid, uid, ppid);
            }
            //add to qleft
            mtree64_qleft(x, tnodeid);
            //fprintf(stdout, "%s::%d %d start:%p state:%p map:%p current:%d left:%d total:%d qleft:%d qfirst:%d qlast:%d\n", __FILE__, __LINE__, id, MT(x)->start, MT(x)->state, MT(x)->map, MT(x)->state->current, MT(x)->state->left, MT(x)->state->total, MT(x)->state->qleft, MT(x)->state->qfirst, MT(x)->state->qlast);
  
        }
        mtree64_mutex_unlock(x, rootid);
    }
    return ;
}

/* remove node */
void mtree64_remove_tnode(void *x, unsigned int tnodeid)
{
    if(x && tnodeid > 0)
    {
        if(MT(x)->map[tnodeid].left > 0 && MT(x)->map[tnodeid].left < MT(x)->state->total)
        {
            mtree64_remove_tnode(x, MT(x)->map[tnodeid].left);
        }
        if(MT(x)->map[tnodeid].right > 0 && MT(x)->map[tnodeid].right < MT(x)->state->total)
        {
            mtree64_remove_tnode(x, MT(x)->map[tnodeid].right);
        }
        mtree64_qleft(x, tnodeid);
    }
    return ;
}

/* remove tree */
void mtree64_remove_tree(void *x, int rootid)
{
    if(x && rootid > 0 && rootid < MTREE64_ROOT_MAX)
    {
        mtree64_mutex_lock(x, rootid);
        mtree64_remove_tnode(x, MT(x)->state->roots[rootid].rootid);
        MT(x)->state->roots[rootid].rootid = 0;
        MT(x)->state->roots[rootid].status = 0;
        //fprintf(stdout, "%s::%d rootid:%d start:%p state:%p map:%p current:%d left:%d total:%d qleft:%d qfirst:%d qlast:%d\n", __FILE__, __LINE__, rootid, MT(x)->start, MT(x)->state, MT(x)->map, MT(x)->state->current, MT(x)->state->left, MT(x)->state->total, MT(x)->state->qleft, MT(x)->state->qfirst, MT(x)->state->qlast);
 
        mtree64_mutex_unlock(x, rootid);
    }
    return ;
}

//close mtree
void mtree64_close(void *x)
{
    int i = 0;
    if(x)
    {
        //fprintf(stdout, "%s::%d start:%p state:%p map:%p current:%d left:%d total:%d qleft:%d qfirst:%d qlast:%d sizeof(MTSTATE64):%d\n", __FILE__, __LINE__, MT(x)->start, MT(x)->state, MT(x)->map, MT(x)->state->current, MT(x)->state->left, MT(x)->state->total, MT(x)->state->qleft, MT(x)->state->qfirst, MT(x)->state->qlast, sizeof(MTSTATE64));
        MT_MUNMAP(x);
        MUTEX_DESTROY(MT(x)->mutex);
#ifdef HAVE_PTHREAD
        for(i = 0; i < MTREE64_MUTEX_MAX; i++)
        {
            pthread_mutex_destroy(&(MT(x)->mutexs[i]));
        }
#endif
        xmm_free(x, sizeof(MTREE64));
    }
    return ;
}


#ifdef _DEBUG_MTREE64
#include "md5.h"
#include "timer.h"
int main(int argc, char **argv) 
{
    int i = 0, rootid = 0, id = 0, j = 0, old = 0, data = 0, n = 0, count = 50000000;
    unsigned char digest[MD5_LEN];
    void *mtree = NULL;
    void *timer = NULL;
    char line[1024];
    int64_t key = 0;

    if((mtree = mtree64_init("/tmp/test.mtree64")))
    {
        rootid = mtree64_new_tree(mtree);
        TIMER_INIT(timer);
        for(j = 1; j <= count; j++)
        {
            n = sprintf(line, "http://www.demo.com/%d.html", j);
            md5(line, n, digest);
            key = *((int64_t *)digest);
            old = -1;
            data = j;
            id = mtree64_insert(mtree, rootid, key, data, &old);
            if(old > 0 || id <= 0) 
            {
                fprintf(stdout, "%d:{id:%d key:%d rootid:%d old:%d}\n", j, id, key, rootid, old);
            }
        }
        TIMER_SAMPLE(timer);
        fprintf(stdout, "%s::%d insert:%d time:%lld\n", __FILE__,__LINE__, count, PT_LU_USEC(timer));
        for(j = 1; j <= count; j++)
        {
            n = sprintf(line, "http://www.demo.com/%d.html", j);
            md5(line, n, digest);
            key = *((int64_t *)digest);
            old = -1;
            data = j;
            id = mtree64_try_insert(mtree, rootid, key, data, &old);
            if(old > 0 && old != j) 
            {
                fprintf(stdout, "%d:{id:%d key:%d rootid:%d old:%d}\n", j, id, key, rootid, old);
                _exit(-1);
            }
        }
        TIMER_SAMPLE(timer);
        fprintf(stdout, "%s::%d try_insert:%d time:%lld\n", __FILE__,__LINE__, count, PT_LU_USEC(timer));
        TIMER_CLEAN(timer);
        mtree64_close(mtree);
    }
}
//gcc -o mtree64 mtree64.c md5.c -D_DEBUG_MTREE64 -g && ./mtree64
#endif
