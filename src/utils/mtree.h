#ifndef _MTREE_H
#define _MTREE_H
#define MTREE_INCRE_NUM    1000000
#define MTREE_NODES_MAX    200000000
#define MTREE_MUTEX_MAX    256
#define MTREE_ROOT_MAX     131072
#include "mutex.h"
typedef struct _MTNODE
{
    unsigned int left;
    unsigned int right;
    unsigned int parent;
    unsigned int color;
    int data;
    int key;
}MTNODE;
typedef struct _MROOT
{
    int status;
    int bits;
    unsigned int total;
    unsigned int rootid;
}MROOT;
typedef struct _MTSTATE
{
    int kmax;
    int kmin;
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
    MROOT roots[MTREE_ROOT_MAX];
}MTSTATE;
typedef struct _MTREE
{
    int fd;
    int status;
    off_t size;
    off_t end;
    off_t old;
    char    *start;
    MTSTATE *state;
    MTNODE  *map;
    MUTEX   mutex;
#ifdef HAVE_PTHREAD
    pthread_mutex_t mutexs[MTREE_MUTEX_MAX];
#endif
}MTREE;
void *mtree_init();
int mtree_new_tree(void *mtree);
unsigned int mtree_total(void *mtree, int rootid);
unsigned int mtree_try_insert(void *mtree, int rootid, int key, int data, int *old);
unsigned int mtree_insert(void *mtree, int rootid, int key, int data, int *old);
unsigned int mtree_get(void *mtree, unsigned int nodeid, int *key, int *data);
unsigned int mtree_find(void *mtree, int rootid, int key, int *data);
unsigned int mtree_min(void *mtree, int rootid, int *key, int *data);
unsigned int mtree_max(void *mtree, int rootid, int *key, int *data);
unsigned int mtree_next(void *mtree, int rootid, unsigned int nodeid, int *key, int *data);
unsigned int mtree_prev(void *mtree, int rootid, unsigned int nodeid, int *key, int *data);
int mtree_set_data(void *mtree, unsigned int nodeid, int data);
void mtree_view_tree(void *mtree, int rootid, FILE *fp);
void mtree_remove(void *mtree, int rootid, unsigned int nodeid, int *key, int *data);
void mtree_remove_tree(void *mtree, int rootid);
void mtree_reuse_all(void *x);
void mtree_close(void *mtree);
#endif
