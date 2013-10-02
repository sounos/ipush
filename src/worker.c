#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <signal.h>
#include <locale.h>
#include <pthread.h>
#include <errno.h>
#include <evbase.h>
#include "common.h"
#include "mmqueue.h"
#include "wtable.h"
#include "logger.h"
#include "xmm.h"
static WTABLE *wtab = NULL;
static int running_status = 0;
static int g_workerid = 0;
static int g_main_worker = 0;
static EVBASE *evbase = NULL;
static LOGGER *logger = NULL;
#define CONN_MAX    65536
#define CONN_BACKLOG_MAX 8192
#define EV_BUF_SIZE 65536
#define CONN_APP_MAX 64
typedef struct _CONN
{
    int status;
    int workerid;
    int keepalive;
    int out_off;
    void *ssl;
    EVENT event;
}CONN;
static CONN *conns = NULL;
static int listenfd = 0;
static int port = 0;
static int is_use_SSL = 0;
/* signal stop */
static void worker_stop(int sig)
{
    switch (sig)
    {
        case SIGINT:
        case SIGTERM:
            if(running_status)
            {/* main thread */
                running_status = 0;
            }
            else
            {/* childs */
                wtab->state->workers[g_workerid].running = W_RUN_STOP;
                MUTEX_SIGNAL(wtab->state->workers[g_workerid].mmlock);
            }
            break;
        default:
            break;
    }
}

void ev_handler(int fd, int ev_flags, void *arg)
{
    struct  sockaddr_in rsa;
    socklen_t rsa_len = sizeof(struct sockaddr_in);
    char line[EV_BUF_SIZE], *p = NULL, *s = NULL, *ss = NULL;
    int rfd = 0, n = 0, nreq = 0, nblock = 0;

    if(fd == listenfd)
    {
        if((ev_flags & E_READ))
        {
            while((rfd = accept(fd, (struct sockaddr *)&rsa, &rsa_len)) > 0)
            {
                memset(&(conns[rfd]), 0, sizeof(CONN));
                fcntl(rfd, F_SETFL, fcntl(rfd, F_GETFL, 0)|O_NONBLOCK);
                event_set(&(conns[rfd].event), rfd, E_READ|E_PERSIST,
                        (void *)&(conns[rfd].event), &ev_handler);
                evbase->add(evbase, &(conns[rfd].event));
                wtab->state->conn_total++;
                fprintf(stdout, "%s::%d conn_total:%d\n", __FILE__, __LINE__, wtab->state->conn_total);
                if(wtab->state->nworkers > 1 
                        && (wtab->state->conn_total/(wtab->state->nworkers-1)) > W_CONN_AVG)
                {
                    wtable_new_task(wtab, g_main_worker, W_CMD_NEWPROC);
                }
            }
        }
    }
    else
    {
        if(ev_flags & E_READ)
        {
            n = read(fd, line, EV_BUF_SIZE);
            fprintf(stdout, "%s::%d read:%d\n", __FILE__, __LINE__, n);
            if(n <= 0) goto err;
            line[n] = 0;
            ss = s = line;
            while((p = strchr(ss, '\n')))
            {
                if(strncmp(s, "{\"appid\":\"", 10) == 0)
                {/* add appid */
                    while(*s != '\0' && *s != '"') s++;
                    if(*s == '"')
                    {
                        fprintf(stdout, "%s::%d appid:%s conn_total:%d\n", __FILE__, __LINE__, ss, wtab->state->conn_total);
                        *s = '\0';
                        wtable_appid(wtab, ss, s - ss);
                    }
                }
                ss = s = ++p;
            }
        }
        if(ev_flags & E_WRITE)
        {
        }
        return ;
err:
        event_destroy(&(conns[fd].event));
        memset(&(conns[fd]), 0, sizeof(CONN));
        shutdown(fd, SHUT_RDWR);
        close(fd);
        --(wtab->state->conn_total);
    }
    return ;
}

/* main worker running */
void worker_running(int wid, int listenport)
{
    WORKER *workers = wtab->state->workers;
    int opt = 1, fd = 0, taskid = 0;
    struct sockaddr_in sa;
    socklen_t sa_len = 0;
    pid_t pid = 0;

#ifdef USE_PTHREAD
        wtable_worker_init(wtab, wid, (int64_t)pthread_self(), W_RUN_WORKING);
#else
        wtable_worker_init(wtab, wid, (int64_t)getpid(), W_RUN_WORKING);
#endif

    wtable_new_task(wtab, g_workerid, W_CMD_NEWPROC);
    wtable_new_task(wtab, g_workerid, W_CMD_NEWPROC);
    /* network settting */
    memset(&sa, 0, sizeof(struct sockaddr_in));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = INADDR_ANY;
    sa.sin_port = htons(listenport);
    sa_len = sizeof(struct sockaddr_in);
    /* Initialize inet */
    listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if(setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR,(char *)&opt, (socklen_t) sizeof(opt)) != 0
#ifdef SO_REUSEPORT
        || setsockopt(listenfd, SOL_SOCKET, SO_REUSEPORT, 
            (char *)&opt, (socklen_t) sizeof(opt)) != 0
#endif
      )
    {
        fprintf(stderr, "setsockopt[SO_REUSEADDR] on fd[%d] failed, %s", listenfd, strerror(errno));
        _exit(-1);
    }
    if(bind(listenfd, (struct sockaddr *)&sa, sa_len) != 0 )
    {
        fprintf(stderr, "Binding failed, %s\n", strerror(errno));
        _exit(-1);
    }
    fprintf(stdout, "%s:%d taskid:%d\n", __FILE__, __LINE__, taskid);
    do
    {
        while((taskid = wtable_pop_task(wtab, g_workerid)) > 0)
        {
            if(taskid == W_CMD_NEWPROC)
            {
                if((pid = fork()) == 0)
                {
                    if(setsid() == -1) exit(EXIT_FAILURE);
                    g_workerid = ++(wtab->state->nworkers);
                    fprintf(stdout, "%s:%d taskid:%d workerid:%d\n", __FILE__, __LINE__, taskid, g_workerid);
                    goto running;
                }
            }
            else if(taskid == W_CMD_STOP) goto exit_running;
        }
        MUTEX_WAIT(workers[wid].mmlock);    
    }while(workers[wid].running == W_RUN_WORKING);
goto exit_running;
running:
#ifdef USE_PTHREAD
        wtable_worker_init(wtab, g_workerid, (int64_t)pthread_self(), W_RUN_WORKING);
#else
        wtable_worker_init(wtab, g_workerid, (int64_t)getpid(), W_RUN_WORKING);
#endif
    /* Listen */
    if(listen(listenfd, CONN_BACKLOG_MAX) != 0 )
    {
        fprintf(stderr, "Listening  failed, %s\n", strerror(errno));
        return;
    }
    if((evbase = evbase_init(0)))
    {
        event_set(&conns[listenfd].event, listenfd, E_READ|E_PERSIST,
                (void *)&conns[listenfd].event, &ev_handler);
        evbase->add(evbase, &(conns[listenfd].event));
        struct timeval tv = {0};
        tv.tv_sec = 0;
        tv.tv_usec = 1000;
        running_status = 1;
        do
        {
            evbase->loop(evbase, 0, &tv);
            while((taskid = wtable_pop_task(wtab, wid)) > 0)
            {
                if(taskid == W_CMD_STOP) goto stop;
            }
        }while(wtab->state->workers[g_workerid].running);
stop:
        event_destroy(&(conns[listenfd].event));
        evbase->clean(evbase);
    }
    close(listenfd);
exit_running:
    wtable_worker_terminate(wtab, g_workerid);
#ifdef USE_PTHREAD
    pthread_exit(NULL);
#else
    exit(0);
#endif
    return ;
}

/* running worker */
void worker_init(void *arg)
{
    WORKER *workers = wtab->state->workers;
    int wid = (int)((long)arg), n = 0, total = 0, taskid = 0;
    char buf[W_BUF_SIZE], *req = NULL;
    pid_t pid = 0;

    if(wid > 0 && wid < W_WORKER_MAX)
    {
#ifdef USE_PTHREAD
        wtable_worker_init(wtab, wid, (int64_t)pthread_self(), W_RUN_WORKING);
#else
        wtable_worker_init(wtab, wid, (int64_t)getpid(), W_RUN_WORKING);
#endif
        wtable_new_task(wtab, wid, W_CMD_NEWPROC);
        do
        {
            while((taskid = wtable_pop_task(wtab, wid)) > 0)
            {
                if(taskid == W_CMD_NEWPROC)
                {
                    if((pid = fork()) == 0)
                    {
                        if(setsid() == -1) exit(EXIT_FAILURE);
                        g_workerid = ++(wtab->state->nworkers);
                        worker_running(g_workerid, port);                
                        goto stop;
                    }
                }
                else if(taskid == W_CMD_STOP) goto stop;
            }
            MUTEX_WAIT(workers[wid].mmlock);    
        }while(workers[wid].running == W_RUN_WORKING);
stop:
        wtable_worker_terminate(wtab, g_workerid);
    }
#ifdef USE_PTHREAD
    pthread_exit(NULL);
#else
    exit(0);
#endif
    return ;
}

/* main worker */
//gcc -o w worker.c wtable.c table.c utils/*.c -I utils -g -lpthread -levbase && ./w 2188 /data/wtab
//gcc -o w worker.c wtable.c table.c utils/*.c -I utils -g -lpthread -levbase -DUSE_PTHREAD && ./w 2188 /data/wtab
int main(int argc, char **argv)
{
    char *workdir = NULL; 
    
    if(argc < 3 || (port = atoi(argv[1])) < 0 || port > 65536)
    {
        fprintf(stderr, "Usage:%s port[1024-65536] workdir\n", argv[0]);
        _exit(-1);
    }
    workdir = argv[2];
    /* locale */
    setlocale(LC_ALL, "C");
    /* signal */
    signal(SIGTERM, &worker_stop);
    signal(SIGINT,  &worker_stop);
    signal(SIGHUP,  &worker_stop);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);
    signal(SIGALRM, SIG_IGN);
    //setrlimiter("RLIMIT_NOFILE", RLIMIT_NOFILE, CONN_MAX);
    //fprintf(stdout, "sizeof(MUTEX):%d %d/%d\n", sizeof(MUTEX), sizeof(pthread_mutex_t), sizeof(pthread_cond_t));
    //fprintf(stdout, "sizeof(CONN):%d\n", sizeof(CONN));
    conns = (CONN *)xmm_mnew(sizeof(CONN) * CONN_MAX);
    if(conns == NULL) exit(EXIT_FAILURE);
    char *log = "/data/worker.log";
    LOGGER_INIT(logger, log);
    if((wtab = wtable_init(workdir)))
    {
        g_workerid = g_main_worker = ++(wtab->state->nworkers); 
        worker_running(g_workerid, port);
        //worker_init((void *)((long )g_workerid));
        if(g_main_worker == g_workerid)
        {
            DEBUG_LOGGER(logger, "ready for stopping %d workers", wtab->state->nworkers);
            wtable_stop(wtab);
            wtable_close(wtab);
        }
    }
    else
    {
        fprintf(stderr, "initialize wtable(%s) failed, %s\n", workdir, strerror(errno));
    }
    //DEBUG_LOGGER(logger, "ready for munmap(%p)", conns);
    if(g_main_worker == g_workerid) xmm_free(conns, sizeof(CONN) * CONN_MAX);
    return 0;
}
