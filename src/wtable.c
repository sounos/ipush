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
#include "mtree64.h"
#include "mmtrie.h"
#include "logger.h"
#include "mutex.h"
#include "xmm.h"
#include "common.h"
#define WT_PATH_MAX         256
#define WT_LOG_NAME         "worker.log"
#define WT_STATE_NAME       "worker.state"
#define WT_MDB_DIR          "mdb"
#define WT_WDB_DIR          "wdb"
#define WT_MAP_NAME         "worker.map"
#define WT_APPMAP_NAME      "worker.appmap"
/* 
 * initialize wtable 
 * */
WTABLE *wtable_init(char *dir)
{
    char path[WT_PATH_MAX], *p = NULL;
    struct stat st = {0};
    int n = 0;

    WTABLE *wtab = NULL;
    if(dir && (wtab = (WTABLE *)xmm_mnew(sizeof(WTABLE))))
    {
        n = sprintf(path, "%s/%s", dir, WT_LOG_NAME);
        force_mkdir(path);
        p = path;
        LOGGER_INIT(wtab->logger, p);
        n = sprintf(path, "%s/%s", dir, WT_WDB_DIR);
        if((wtab->wdb = db_init(path, 1)) == NULL) _exit(-1);
        n = sprintf(path, "%s/%s", dir, WT_MDB_DIR);
        /* state */
        n = sprintf(path, "%s/%s", dir, WT_STATE_NAME);
        if((wtab->statefd = open(path, O_CREAT|O_RDWR, 0644)) <= 0
                || fstat(wtab->statefd, &st) != 0)
        {
            if(wtab->statefd > 0) close(wtab->statefd);
            FATAL_LOGGER(wtab->logger, "open state file[%s] failed, %s",
                    path, strerror(errno));
            _exit(-1);
        }
        else
        {
            if(st.st_size < sizeof(WSTATE)
                    && ftruncate(wtab->statefd, sizeof(WSTATE)) != 0)
            {
                _exit(-1);
            }
            if((wtab->state = (WSTATE *)mmap(NULL, sizeof(WSTATE),
                            PROT_READ|PROT_WRITE, MAP_SHARED, wtab->statefd, 0)) == NULL
                    || wtab->state == (void *)-1)
            {
                FATAL_LOGGER(wtab->logger, "mmap state failed, %s", strerror(errno));
                _exit(-1);
            }
            if(st.st_size < sizeof(WSTATE))
                memset(((char *)wtab->state + st.st_size), 0, sizeof(WSTATE) - st.st_size);
        }
        memset(wtab->state->workers, 0, sizeof(WORKER) * W_WORKER_MAX);
        wtab->state->nworkers = 0;
        wtab->state->conn_total = 0;
        /* mmtrie */
        n = sprintf(path, "%s/%s", dir, WT_MAP_NAME);
        if((wtab->map = mmtrie_init(path)) == NULL) _exit(-1);
        /* appmap */
        n = sprintf(path, "%s/%s", dir, WT_APPMAP_NAME);
        if((wtab->appmap = mmtree64_init(path)) == NULL) _exit(-1);
        /* logger & mutex & mtree & mqueue */
        if((wtab->queue = mqueue_init()) == NULL) _exit(-1);
        if((wtab->mtree = mtree64_init()) == NULL) _exit(-1);
        wtab->whitelist = mtree64_new_tree(wtab->mtree);
        //MQ(wtab->queue)->logger = wtab->logger;
        MUTEX_INIT(wtab->mutex);
    }
    return wtab;
}

/* add whitelist ip*/
int wtable_set_whitelist(WTABLE *wtab, int ip)
{
    int ret = -1;
    if(wtab && ip)
    {
        ret = mtree64_try_insert(wtab->mtree, wtab->whitelist, (int64_t)ip, ip, NULL);
    }
    return ret;
}

/* check whitelist */
int wtable_check_whitelist(WTABLE *wtab, int ip)
{
    int ret = -1;
    if(wtab && ip)
    {
        ret = mtree64_find(wtab->mtree, wtab->whitelist, (int64_t)ip, NULL);
    }
    return ret;
}
/* worker  init */
int wtable_worker_init(WTABLE *wtab, int workerid, int64_t childid, int status)
{
    if(wtab && workerid > 0 && workerid < W_WORKER_MAX && childid)
    {
        wtab->state->workers[workerid].msg_qid = mqueue_new(wtab->queue);
        wtab->state->workers[workerid].conn_qid = mtree64_new_tree(wtab->mtree);
        wtab->state->workers[workerid].task_qid = mqueue_new(wtab->queue);
        wtab->state->workers[workerid].childid = childid;
        wtab->state->workers[workerid].running = status;
        MUTEX_INIT(wtab->state->workers[workerid].mmlock);
        DEBUG_LOGGER(wtab->logger, "init workers[%d] msg_qid[%d]",workerid,wtab->state->workers[workerid].msg_qid);
    }
    return 0;
}

int wtable_new_task(WTABLE *wtab, int workerid, int taskid)
{
    int ret = 0;

    if(wtab && workerid > 0 && workerid < W_WORKER_MAX)
    {
        mqueue_push(wtab->queue, wtab->state->workers[workerid].task_qid, taskid);
        MUTEX_SIGNAL(wtab->state->workers[workerid].mmlock);
    }
    return ret;
}

int wtable_pop_task(WTABLE *wtab, int workerid)
{
    int ret = -1;

    if(wtab && workerid > 0 && workerid < W_WORKER_MAX)
    {
        mqueue_pop(wtab->queue, wtab->state->workers[workerid].task_qid, &ret);
    }
    return ret;
}

/* wtable new app */
int wtable_appid(WTABLE *wtab, char *appkey, int len)
{
    int appid = -1;
    if(wtab && appkey && len > 0)
    {
        MUTEX_LOCK(wtab->mutex);
        appid = mmtrie_xadd(wtab->map, appkey, len);
        if(appid > wtab->state->app_id_max)
        {
            wtab->state->app_id_max = appid;
            appid = mmtree64_new_tree(wtab->appmap);
        }
        MUTEX_UNLOCK(wtab->mutex);
    }
    return appid;
}

/* wtable app auth */
int wtable_appid_auth(WTABLE *wtab, int wid, char *appkey, int len, int conn_id)
{
    int appid = -1;
    if(wtab && appkey && len > 0 && (appid = mmtrie_get(wtab->map, appkey, len)) > 0)     
    {
        mtree64_insert(wtab->mtree, wtab->state->workers[wid].conn_qid, appid, conn_id, NULL); 
    }
    return appid;
}

/* wtable new push msg */
int wtable_new_msg(WTABLE *wtab, int appid, char *msg, char len)
{
    int msgid = 0, mid = 0, i = 0, id = 0;
    struct timeval tv = {0};
    char buf[W_BUF_SIZE];
    int64_t now = 0;
    WHEAD *head = (WHEAD *)buf;

    if(wtab && appid > 0 && msg && len > 0)
    {
        msgid = ++(wtab->state->msg_id_max);
        head->mix = appid;
        head->len = len;
        strncpy(buf + sizeof(WHEAD), msg, len);
        db_set_data(wtab->mdb, msgid, buf, len + sizeof(WHEAD)); 
        gettimeofday(&tv, NULL);now = (int64_t)tv.tv_sec * 1000000 + (int64_t)tv.tv_usec;
        mid = (int)mmtree64_try_insert(wtab->appmap, appid, now, msgid, NULL);
        for(i = 0; i < wtab->state->nworkers; i++)
        {
            mqueue_push(wtab->queue, wtab->state->workers[i].msg_qid, mid);
        }
    }
    return msgid;
}

/* get msg info */
int wtable_get_msg(WTABLE *wtab, int workerid, char **block)
{
    int mid = 0, ret = -1, msgid = 0;
    int64_t time = 0;

    if(wtab && workerid > 0 && block)
    {
        if((mqueue_pop(wtab->queue, wtab->state->workers[workerid].msg_qid, &mid))
                && mmtree64_get(wtab->appmap, (unsigned int )mid, &time, &msgid) )
        {
            ret = db_exists_block(wtab->mdb, msgid, block);
        }
    }
    return ret;
}

/* get msg list */
int wtable_get_msgs(WTABLE *wtab, int appid, int64_t last_time, char ***blocks)
{
    int ret = 0, mid = 0, msgid = 0, n = 0;
    int64_t time = 0;

    if(wtab && appid && last_time && blocks) 
    {
        mid = mmtree64_max(wtab->appmap, appid, &time, &msgid);
        while(mid && time >= last_time && msgid > 0)
        {
            n = db_exists_block(wtab->mdb, msgid, blocks[ret++]);
            time = 0, msgid = 0;
            mid = mmtree64_prev(wtab->appmap, appid, mid, &time, &msgid);
        }
    }
    return ret;
}

int wtable_stop(WTABLE *wtab)
{
    int ret = -1, i = 0;

    if(wtab) 
    {
        DEBUG_LOGGER(wtab->logger, "ready for stopping %d workers", wtab->state->nworkers);
        for(i = 1; i <= wtab->state->nworkers; i++)
        {
            if(wtab->state->workers[i].running == W_RUN_WORKING)
            {
                DEBUG_LOGGER(wtab->logger, "stopping workers[%d] => %d", i, wtab->state->workers[i].childid);
                wtab->state->workers[i].running = W_RUN_STOP;
                wtable_new_task(wtab, i, W_CMD_STOP);
#ifdef USE_PTHREAD
        pthread_join((pthread_t)(wtab->state->workers[i].childid), NULL);
#else
        waitpid((pid_t)wtab->state->workers[i].childid, NULL, 0);
#endif
                DEBUG_LOGGER(wtab->logger, "stop workers[%d] => %d", i, wtab->state->workers[i].childid);
            }
        }
        DEBUG_LOGGER(wtab->logger, "stop %d workers", wtab->state->nworkers);
        ret = 0;
    }
    return ret;
}

/* worker  terminate */
int wtable_worker_terminate(WTABLE *wtab, int workerid)
{
    if(wtab && workerid >= 0)
    {
        DEBUG_LOGGER(wtab->logger, "terminate workers[%d] childid:%lld", workerid, wtab->state->workers[workerid].childid);
        MUTEX_DESTROY(wtab->state->workers[workerid].mmlock);
        memset(&(wtab->state->workers[workerid]), 0, sizeof(WORKER));
    }
    return 0;
}

/*
 * close/clean wtable
 * */
void wtable_close(WTABLE *wtab)
{
    if(wtab)
    {
        if(wtab->statefd > 0) close(wtab->statefd);
        wtab->statefd = 0;
        if(wtab->state) munmap(wtab->state, sizeof(WSTATE));
        wtab->state = NULL;
        db_clean(wtab->wdb);
        db_clean(wtab->mdb);
        mqueue_clean(MQ(wtab->queue));
        mmtrie_clean(wtab->map);
        mtree64_close(wtab->mtree);
        mmtree64_close(wtab->appmap);
        LOGGER_CLEAN(wtab->logger);
        xmm_free(wtab, sizeof(WTABLE));
    }
    return ;
}

#ifdef __DEBUG__WTABLE
// gcc -o wtab wtable.c -I utils/ utils/*.c -g -D__DEBUG__WTABLE && ./wtab
int main(int argc, char **argv)
{
    WTABLE *wtab = NULL;

    if((wtab = wtable_init("/data/wtab")))
    {
        wtable_close(wtab);
    }
    return 0;
}
#endif
