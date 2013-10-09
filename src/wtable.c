#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <signal.h>
#include <locale.h>
#include "wtable.h"
#include "db.h"
#include "mqueue.h"
#include "mmtree64.h"
#include "mtree.h"
#include "mmtrie.h"
#include "logger.h"
#include "mutex.h"
#include "xmm.h"
#include "common.h"
#define WT_PATH_MAX         256
#define WT_LOG_NAME         "worker.log"
#define WT_STATE_NAME       "worker.state"
#define WT_MDB_DIR          "mdb"
#define WT_MAP_NAME         "worker.map"
#define WT_APPMAP_NAME      "worker.appmap"
/* 
 * initialize wtable 
 * */
WTABLE *wtable_init(char *dir)
{
    char path[WT_PATH_MAX], *p = NULL;
    struct stat st = {0};
    WTABLE *w = NULL;
    int n = 0;

    if(dir && (w = (WTABLE *)xmm_mnew(sizeof(WTABLE))))
    {
        n = sprintf(path, "%s/%s", dir, WT_LOG_NAME);
        force_mkdir(path);
        p = path;
        LOGGER_INIT(w->logger, p);
        /* state */
        n = sprintf(path, "%s/%s", dir, WT_STATE_NAME);
        if((w->statefd = open(path, O_CREAT|O_RDWR, 0644)) <= 0
                || fstat(w->statefd, &st) != 0)
        {
            if(w->statefd > 0) close(w->statefd);
            FATAL_LOGGER(w->logger, "open state file[%s] failed, %s",
                    path, strerror(errno));
            _exit(-1);
        }
        else
        {
            if(st.st_size < sizeof(WSTATE)
                    && ftruncate(w->statefd, sizeof(WSTATE)) != 0)
            {
                _exit(-1);
            }
            if((w->state = (WSTATE *)mmap(NULL, sizeof(WSTATE),
                            PROT_READ|PROT_WRITE, MAP_SHARED, w->statefd, 0)) == NULL
                    || w->state == (void *)-1)
            {
                FATAL_LOGGER(w->logger, "mmap state failed, %s", strerror(errno));
                _exit(-1);
            }
            if(st.st_size < sizeof(WSTATE))
                memset(((char *)w->state + st.st_size), 0, sizeof(WSTATE) - st.st_size);
        }
        w->workers = w->state->workers;
        memset(w->workers, 0, sizeof(WORKER) * W_WORKER_MAX);
        w->state->nworkers = 0;
        w->state->conn_total = 0;
        n = sprintf(path, "%s/%s", dir, WT_MDB_DIR);
        w->mdb = db_init(path, DB_USE_MMAP);
        /* mmtrie */
        n = sprintf(path, "%s/%s", dir, WT_MAP_NAME);
        if((w->map = mmtrie_init(path)) == NULL) _exit(-1);
        /* appmap */
        n = sprintf(path, "%s/%s", dir, WT_APPMAP_NAME);
        if((w->appmap = mmtree64_init(path)) == NULL) _exit(-1);
        mmtree64_use_all(w->appmap);
        /* logger & mutex & mtree & mqueue */
        if((w->queue = mqueue_init()) == NULL) _exit(-1);
        if((w->mtree = mtree_init()) == NULL) _exit(-1);
        w->whitelist = mtree_new_tree(w->mtree);
        //MQ(w->queue)->logger = w->logger;
        MUTEX_INIT(w->mutex);
    }
    return w;
}

/* add whitelist ip*/
int wtable_set_whitelist(WTABLE *w, int ip)
{
    int ret = -1;
    if(w && ip)
    {
        ret = mtree_try_insert(w->mtree, w->whitelist, ip, ip, NULL);
    }
    return ret;
}

/* check whitelist */
int wtable_check_whitelist(WTABLE *w, int ip)
{
    int ret = -1;
    if(w && ip)
    {
        ret = mtree_find(w->mtree, w->whitelist, ip, NULL);
    }
    return ret;
}

/* worker  init */
int wtable_worker_init(WTABLE *w, int wid, int64_t childid, int status)
{
    if(w && wid > 0 && wid < W_WORKER_MAX && childid)
    {
        memset(&(w->workers[wid]), 0, sizeof(WORKER));
        w->workers[wid].msg_qid = mqueue_new(w->queue);
        w->workers[wid].task_qid = mqueue_new(w->queue);
        w->workers[wid].childid = childid;
        w->workers[wid].running = status;
        if(wid > 1) 
        {
            w->workers[wid].queue = mqueue_init();
            w->workers[wid].map = mtree_init();
            mtree_reuse_all(w->workers[wid].map);
        }
        MUTEX_INIT(w->workers[wid].mmlock);
        DEBUG_LOGGER(w->logger, "init workers[%d] msg_qid[%d]",wid,w->workers[wid].msg_qid);
    }
    return 0;
}

/* new connection */
int wtable_newconn(WTABLE *w, int wid, int id)
{
    int ret = -1;

    if(w && wid > 0 && id > 0 && id < W_CONN_MAX)
    {
        ret = w->workers[wid].q[id] = mqueue_new(w->workers[wid].queue);
        //REALLOG(w->logger, "workers[%d] new-conn:%d", wid, id);
    }
    return ret;
}

/* end connection */
int wtable_endconn(WTABLE *w, int wid, int id, int *apps, int apps_num)
{
    int ret = -1, i = 0, appid = 0, mid = 0;
    WORKER *workers = NULL;

    if(w && wid > 0 && id > 0 && id < W_CONN_MAX && apps 
            && (workers = w->workers) )
    {
        for(i = 0; i < apps_num; i++)
        {
            if((appid = apps[i]) > 0)
            {
                if((mid = mtree_find(workers[wid].map,appid,id,NULL))>0) 
                    mtree_remove(workers[wid].map,appid,mid,NULL,NULL);
                //REALLOG(w->logger, "app:%d left:%d mid:%d conn:%d", appid, mtree_total(workers[wid].map,appid), mid, id);
            }
        }
        //REALLOG(w->logger, "conn[%d] apps:%d", id, mqueue_total(workers[wid].queue, workers[wid].q[id]));
        ret = mqueue_close(workers[wid].queue, workers[wid].q[id]);
        w->workers[wid].q[id] = 0;
    }
    return ret;
}

/* push new taskid */
int wtable_new_task(WTABLE *w, int wid, int taskid)
{
    int ret = 0;

    if(w && wid > 0 && wid < W_WORKER_MAX)
    {
        mqueue_push(w->queue, w->workers[wid].task_qid, taskid);
        MUTEX_SIGNAL(w->workers[wid].mmlock);
    }
    return ret;
}

/* get first taskid */
int wtable_pop_task(WTABLE *w, int wid)
{
    int ret = -1;

    if(w && wid > 0 && wid < W_WORKER_MAX)
    {
        mqueue_pop(w->queue, w->workers[wid].task_qid, &ret);
    }
    return ret;
}

/* wtable new app */
int wtable_appid(WTABLE *w, char *appkey, int len)
{
    int appid = -1;
    if(w && appkey && len > 0)
    {
        MUTEX_LOCK(w->mutex);
        appid = mmtrie_xadd(w->map, appkey, len);
        if(appid > w->state->app_id_max)
        {
            w->state->app_id_max = appid;
        }
        MUTEX_UNLOCK(w->mutex);
    }
    return appid;
}

/* wtable app auth */
int wtable_app_auth(WTABLE *w, int wid, char *appkey, int len, int conn_id, int64_t last_time)
{
    int mid = 0, msgid = 0, appid = 0;
    int64_t time = 0;

    if(w && appkey && len > 0 && (appid = mmtrie_get(w->map, appkey, len)) > 0)     
    {
        mtree_insert(w->workers[wid].map, appid, conn_id, wid, NULL);
        //REALLOG(w->logger, "workers[%d] app[%.*s][%d][%d] qtotal:%p/%d", wid, len, appkey, appid, conn_id, w->workers[wid].map, mtree_total(w->workers[wid].map, appid));
        mid = mmtree64_max(w->appmap, appid, &time, &msgid);
        while(mid && time >= last_time && msgid > 0)
        {
            mqueue_push(w->workers[wid].queue,w->workers[wid].q[conn_id],msgid);
            time = 0; msgid = 0;
            mid = mmtree64_prev(w->appmap, appid, mid, &time, &msgid);
        }
    }
    return appid;
}

/* wtable new push msg */
int wtable_new_msg(WTABLE *w, int appid, char *msg, int len)
{
    int msgid = 0, i = 0;
    struct timeval tv = {0};
    char buf[W_BUF_SIZE];
    int64_t now = 0;
    WHEAD *head = (WHEAD *)buf;

    if(w && appid > 0 && msg && len > 0)
    {
        msgid = ++(w->state->msg_id_max);
        head->mix = appid;
        head->len = len;
        strncpy(buf + sizeof(WHEAD), msg, len);
        db_set_data(w->mdb, msgid, buf, len + sizeof(WHEAD)); 
        gettimeofday(&tv, NULL);now = (int64_t)tv.tv_sec * 1000000 + (int64_t)tv.tv_usec;
        mmtree64_try_insert(w->appmap, appid, now, msgid, NULL);
        for(i = 2; i <= w->state->nworkers; i++)
        {
            mqueue_push(w->queue, w->workers[i].msg_qid, msgid);
        }
        //REALLOG(w->logger, "new-msg[%.*s] for appid:%d", len, msg, appid);
    }
    return msgid;
}

/* get msg info */
int wtable_get_msg(WTABLE *w, int wid, int conn_id, char **msg)
{
    int ret = -1, msgid = 0;
    WHEAD *head = NULL;

    if(w && wid > 0 && conn_id > 0 && conn_id < W_CONN_MAX && msg)
    {
        if(mqueue_head(w->workers[wid].queue, w->workers[wid].q[conn_id], &msgid) > 0)
        {
            ret = db_exists_block(w->mdb, msgid, (char **)&head) - sizeof(WHEAD);
            if(ret) *msg = (char *)head + sizeof(WHEAD);
        }
    }
    return ret;
}

/* over msg */
int wtable_over_msg(WTABLE *w, int wid, int conn_id)
{
    int ret = -1;

    if(w && wid > 0 && conn_id > 0 && conn_id < W_CONN_MAX)
    {
        mqueue_pop(w->workers[wid].queue, w->workers[wid].q[conn_id], NULL);
        ret = mqueue_total(w->workers[wid].queue, w->workers[wid].q[conn_id]);
    }
    return ret;
}

int wtable_stop(WTABLE *w)
{
    int ret = -1, i = 0;

    if(w) 
    {
        DEBUG_LOGGER(w->logger, "ready for stopping %d workers", w->state->nworkers);
        for(i = 1; i <= w->state->nworkers; i++)
        {
            if(w->workers[i].running == W_RUN_WORKING)
            {
                DEBUG_LOGGER(w->logger, "stopping workers[%d] => %d", i, w->workers[i].childid);
                w->workers[i].running = W_RUN_STOP;
                wtable_new_task(w, i, W_CMD_STOP);
#ifdef USE_PTHREAD
        pthread_join((pthread_t)(w->workers[i].childid), NULL);
#else
        waitpid((pid_t)w->workers[i].childid, NULL, 0);
#endif
                DEBUG_LOGGER(w->logger, "stop workers[%d] => %d", i, w->workers[i].childid);
            }
        }
        DEBUG_LOGGER(w->logger, "stop %d workers", w->state->nworkers);
        ret = 0;
    }
    return ret;
}

/* worker  terminate */
int wtable_worker_terminate(WTABLE *w, int wid)
{
    if(w && wid >= 0)
    {
        DEBUG_LOGGER(w->logger, "terminate workers[%d] childid:%lld", wid, w->workers[wid].childid);
        MUTEX_DESTROY(w->workers[wid].mmlock);
        mqueue_clean(w->workers[wid].queue);
        mtree_close(w->workers[wid].map);
        memset(&(w->workers[wid]), 0, sizeof(WORKER));
    }
    return 0;
}

/*
 * close/clean wtable
 * */
void wtable_close(WTABLE *w)
{
    if(w)
    {
        if(w->statefd > 0) close(w->statefd);
        w->statefd = 0;
        if(w->state) munmap(w->state, sizeof(WSTATE));
        w->state = NULL;
        db_clean(w->mdb);
        mqueue_clean(MQ(w->queue));
        mmtrie_clean(w->map);
        mtree_close(w->mtree);
        mmtree64_close(w->appmap);
        LOGGER_CLEAN(w->logger);
        xmm_free(w, sizeof(WTABLE));
    }
    return ;
}

#ifdef __DEBUG__WTABLE
// gcc -o w wtable.c -I utils/ utils/*.c -g -D__DEBUG__WTABLE && ./wtab
int main(int argc, char **argv)
{
    WTABLE *w = NULL;

    if((w = wtable_init("/data/wtab")))
    {
        wtable_close(wtab);
    }
    return 0;
}
#endif
