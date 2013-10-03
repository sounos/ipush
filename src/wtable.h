#ifndef __WTABLE__H__
#define __WTABLE__H__
#include "mutex.h"
#define W_WORKER_MAX        4096
#define W_CMD_NEWPROC       0x01
#define W_CMD_STOP          0x02
#define W_CMD_TASK          0x04
#define W_CMD_RELOAD        0x08
#define W_CMD_PUSH          0x10
#define W_CMD_MAX           32
#define W_RUN_WORKING       0x01
#define W_RUN_STOP          0x00
#define W_STATUS_OK         1
#define W_STATUS_ERR        -1
#define W_STATUS_INIT       0
#define W_PATH_MAX          256
#define W_CONN_AVG          20480
#define W_LINE_SIZE         8192
#define W_BUF_SIZE          65536
typedef struct _WORKER
{
    int         conn_qid;
    int         msg_qid;
    int         task_qid;
    int         running;
    int64_t     childid;
    MUTEX       mmlock;
}WORKER;
typedef struct _WHEAD
{
    int mix;
    int len;
}WHEAD;
typedef struct _WSTATE
{
    int  nworkers;
    int  conn_total;
    int  app_id_max;
    int  msg_id_max;
    int  whitelist;
    int  bits;
    WORKER workers[W_WORKER_MAX];
    char dir[W_PATH_MAX];
}WSTATE;
typedef struct _WTABLE
{
    int  statefd;
    int  bits;
    MUTEX mutex;
    void *mdb;/* msg db */
    void *wdb;/* worker db */
    void *map;/* key/value map */
    void *appmap;/* mmtree */
    void *mtree;/* mtree */
    void *queue;/* msg queue*/
    void *task_queue;/* task queue*/
    void *logger;
    WSTATE *state; 
}WTABLE;
WTABLE *wtable_init(char *dir);
int wtable_set_whitelist(WTABLE *wtab, int ip);
int wtable_check_whitelist(WTABLE *wtab, int ip);
int wtable_worker_init(WTABLE *wtab, int workerid, int64_t childid, int status);
int wtable_new_task(WTABLE *wtab, int workerid, int taskid);
int wtable_pop_task(WTABLE *wtab, int workerid);
int wtable_appid(WTABLE *wtab, char *appkey, int len);
int wtable_appid_auth(WTABLE *wtab, int wid, char *appkey, int len, int conn_id);
int wtable_new_msg(WTABLE *wtab, int appid, char *msg, char len);
int wtable_get_msg(WTABLE *wtab, int workerid, char **block);
int wtable_get_msgs(WTABLE *wtab, int appid, int64_t last_time, char ***blocks);
int wtable_stop(WTABLE *wtab);
int wtable_worker_terminate(WTABLE *wtab, int workerid);
void wtable_close(WTABLE *wtab);
#endif
