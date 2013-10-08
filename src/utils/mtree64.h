#ifndef _MTREE64_H
#define _MTREE64_H
#define MTREE64_INCRE_NUM    1000000
#define MTREE64_NODES_MAX    200000000
#define MTREE64_MUTEX_MAX    256
#define MTREE64_ROOT_MAX     131072
#include "mutex.h"
typedef struct _MTNODE64
{
    unsigned int left;
    unsigned int right;
    unsigned int parent;
    unsigned int color;
    int data;
    int bit;
    int64_t key;
}MTNODE64;
typedef struct _MROOT64
{
    int status;
    int bits;
    unsigned int total;
    unsigned int rootid;
}MROOT64;
typedef struct _MTSTATE64
{
    int64_t kmax;
    int64_t kmin;
    unsigned int nmax;
    unsigned int nmin;
    unsigned int count;
    unsigned int left;
    unsigned int current;
    unsigned int total;
    unsigned int qleft;
    unsigned int qfirst;
    unsigned int qlast;
    unsigned int nroots;
    MROOT64 roots[MTREE64_ROOT_MAX];
}MTSTATE64;
typedef struct _MTREE64
{
    int fd;
    int status;
    off_t size;
    off_t end;
    off_t old;
    char    *start;
    MTSTATE64 *state;
    MTNODE64  *map;
    MUTEX   mutex;
#ifdef HAVE_PTHREAD
    pthread_mutex_t mutexs[MTREE64_MUTEX_MAX];
#endif
}MTREE64;
void *mtree64_init();
int mtree64_new_tree(void *mtree);
unsigned int mtree64_total(void *mtree, int rootid);
unsigned int mtree64_try_insert(void *mtree, int rootid, int64_t key, int data, int *old);
unsigned int mtree64_insert(void *mtree, int rootid, int64_t key, int data, int *old);
unsigned int mtree64_get(void *mtree, unsigned int nodeid, int64_t *key, int *data);
unsigned int mtree64_find(void *mtree, int rootid, int64_t key, int *data);
unsigned int mtree64_min(void *mtree, int rootid, int64_t *key, int *data);
unsigned int mtree64_max(void *mtree, int rootid, int64_t *key, int *data);
unsigned int mtree64_next(void *mtree, int rootid, unsigned int nodeid, int64_t *key, int *data);
unsigned int mtree64_prev(void *mtree, int rootid, unsigned int nodeid, int64_t *key, int *data);
int mtree64_set_data(void *mtree, unsigned int nodeid, int data);
void mtree64_view_tree(void *mtree, int rootid, FILE *fp);
void mtree64_remove(void *mtree, int rootid, unsigned int nodeid, int64_t *key, int *data);
void mtree64_remove_tree(void *mtree, int rootid);
void mtree64_close(void *mtree);
#endif
