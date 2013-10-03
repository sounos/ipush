#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <getopt.h> 
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
    ushort port;
    ushort bit;
    int bits;
    char ip[16];
    void *ssl;
    EVENT event;
}CONN;
static CONN *conns = NULL;
static int listenfd = 0;
static int port = 0;
static int is_use_SSL = 0;
static char *cert = NULL;
static char *privkey = NULL;
/* signal stop */
static void worker_stop(int sig)
{
    switch (sig)
    {
        case SIGINT:
        case SIGTERM:
            /* childs */
            wtab->state->workers[g_workerid].running = W_RUN_STOP;
            MUTEX_SIGNAL(wtab->state->workers[g_workerid].mmlock);
            break;
        default:
            break;
    }
}

void ev_handler(int fd, int ev_flags, void *arg)
{
    struct  sockaddr_in rsa;
    socklen_t rsa_len = sizeof(struct sockaddr_in);
    char line[EV_BUF_SIZE], *p = NULL, *s = NULL, *ss = NULL, *xs = NULL;
    int rfd = 0, n = 0, nreq = 0, nblock = 0, appid = 0, msgid = 0;
    int64_t last = 0;

    if(fd == listenfd)
    {
        if((ev_flags & E_READ))
        {
            while((rfd = accept(fd, (struct sockaddr *)&rsa, &rsa_len)) > 0)
            {
                memset(&(conns[rfd]), 0, sizeof(CONN));
                strcpy(conns[rfd].ip, inet_ntoa(rsa.sin_addr));
                conns[rfd].port = ntohs(rsa.sin_port);
                REALLOG(logger, "new connection[%s:%d] via %d", conns[rfd].ip, conns[rfd].port, rfd)
                event_set(&(conns[rfd].event), rfd, E_READ|E_PERSIST,
                        (void *)&(conns[rfd].event), &ev_handler);
                evbase->add(evbase, &(conns[rfd].event));
                ++(wtab->state->conn_total);
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
            if(n <= 0) goto err;
            line[n] = 0;
            ss = s = line;
            while((p = strchr(ss, '\n')))
            {
                if(strncmp(s, "{\"appid\":\"", 10) == 0)
                {
                    if(wtable_check_whitelist(wtab, inet_addr(conns[fd].ip)) > 0)
                    {/* check whitelist & add appid */
                        s += 10;
                        xs = s;
                        while(*s != '\0' && *s != '"') ++s;
                        if(*s == '"')
                        {
                            *s = '\0';
                            appid = wtable_appid(wtab, xs, s - xs);
                            REALLOG(logger, "new appkey[%s] from conn[%s:%d] via %d", ss, conns[fd].ip, conns[fd].port, fd)
                            *s = '"';
                        }
                    }
                    else 
                    {
                        WARN_LOGGER(logger, "no whitelist conn[%s:%d] via %d", conns[fd].ip, conns[fd].port, fd)
                        goto err;
                    }
                }
                else if(strncmp(s, "{\"time\":\"", 9) == 0)
                {
                    if((s = strstr(s, "\"oauth_key\":\"")) && wtable_check_whitelist(wtab, inet_addr(conns[fd].ip)) > 0)
                    {/* check whitelist & add msg */
                        s += 11;
                        xs = s;
                        while(*s != '\0' && *s != '"') ++s;
                        if(*s == '"')
                        {
                            *s = '\0';
                            appid = wtable_appid(wtab, xs, s - xs);
                            *s = '"';
                            n = (p+1) - ss;
                            msgid = wtable_new_msg(wtab, appid, ss, n);
                            REALLOG(logger, "new msg[%.*s] from conn[%s:%d] via %d", n, ss, conns[fd].ip, conns[fd].port, fd)
                        }
                    }
                    else 
                    {
                        WARN_LOGGER(logger, "no whitelist conn[%s:%d] via %d", conns[fd].ip, conns[fd].port, fd)
                        goto err;
                    }
                }
                else if(strncmp(s, "{\"last\":\"", 9) == 0)
                {
                    s += 9;
                    xs = s;
                    while(*s != '\0' && *s != '"') ++s;
                    if(*s == '"')
                    {
                        *s = '\0';
                        last = strtotime64(s);
                        *s = '"';
                    }
                    if((s = strstr(s, "\"oauth_key\":\"")))
                    {/* auth key  */
                        s += 11;
                        xs = s;
                        while(*s != '\0' && *s != '"') ++s;
                        if(*s == '"')
                        {
                            *s = '\0';
                            if((appid = wtable_appid_auth(wtab, g_workerid, xs, s - xs, fd)) < 1) 
                            {
                                WARN_LOGGER(logger, "unknown appkey[%s] from conn[%s:%d] via %d", ss, conns[fd].ip, conns[fd].port, fd)
                                goto err;
                            }
                            /* set applist */
                            *s = '"';
                        }
                    }
                    else 
                    {
                        WARN_LOGGER(logger, "bad request[%s] from conn[%s:%d] via %d", line, conns[fd].ip, conns[fd].port, fd)
                        goto err;
                    }
                }
                else goto err;
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
    fcntl(listenfd, F_SETFL, fcntl(listenfd, F_GETFL, 0)|O_NONBLOCK);
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
                    goto running;
                }
            }
            else if(taskid == W_CMD_STOP) return ;
        }
        MUTEX_WAIT(workers[g_workerid].mmlock);    
    }while(workers[g_workerid].running == W_RUN_WORKING);
return ;
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
    char log[256], *ss = NULL, *s = NULL, *workdir = NULL, *whitelist = NULL, 
         c = 0, *short_options= "sdw:p:b:c:k:"; 
    struct option long_options[] = {
        {"ssl",0,NULL,'s'},
        {"daemon",0,NULL,'d'},
        {"workdir",1,NULL,'b'},
        {"port",1,NULL,'p'},
        {"whitelist",1,NULL,'w'},
        {"cert",1,NULL,'c'},
        {"privkey",1,NULL,'k'},
        {NULL,0,NULL,0}
    };
    int is_run_daemon = 0, n = 0;
    while((c = getopt_long(argc, argv, short_options, long_options, NULL)) != -1)  
    {  
        switch (c)  
        {  
            case 's':  
#ifdef HAVE_SSL
                is_use_SSL  = 1;
#else
                fprintf(stderr, "no SSL library found OR no compie option:-DHAVE_SSL\n");
            _exit(-1);
#endif
                break;  
            case 'd':  
                is_run_daemon = 1;
                break;
            case 'b':  
                workdir = optarg;
                break;  
            case 'p':  
                port = atoi(optarg);
                break;  
            case 'w':  
                whitelist = optarg;
                break;  
            case 'c':
                cert = optarg;
                break;  
            case 'k':
                privkey = optarg;
                break;  
            default :
                break;
        }  
    } 
    if(!port || !whitelist || !workdir || !cert || !privkey)
    {
        fprintf(stderr, "Usage:%s --port=listenport\n"
                "\t--whitelist=ip1,ip2,ip3...\n"
                "\t--workdir=workdir\n"
                "\t--cert=cert\n"
                "\t--privkey=privkey\n"
                "\t--ssl(use-ssl)\n"
                "\t--daemon(run as daemon)\n", argv[0]);
        _exit(-1);
    }
    if(is_run_daemon)
    {
        pid_t pid = fork();
        switch (pid) {
            case -1:
                perror("fork()");
                exit(EXIT_FAILURE);
                break;
            case 0: //child
                if(setsid() == -1) exit(EXIT_FAILURE);
                break;
            default://parent
                _exit(EXIT_SUCCESS);
                break;
        }
    }
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
    conns = (CONN *)xmm_mnew(sizeof(CONN) * CONN_MAX);
    if(conns == NULL) exit(EXIT_FAILURE);
    if((wtab = wtable_init(workdir)))
    {
        sprintf(log, "%s/run.log", workdir);
        LOGGER_INIT(logger, log);
        ss = s = whitelist;
        while(*s != '\0')
        {
            if(*s == ',')
            {
                *s = '\0';
                if((s - ss) > 0)
                {
                    wtable_set_whitelist(wtab, (int)inet_addr(ss));
                    REALLOG(logger, "added whitelist:%s", ss);
                }
                ss = ++s;
            }
            else ++s;
        }
        if((s - ss) > 0)
        {
            wtable_set_whitelist(wtab, (int)inet_addr(ss));
            REALLOG(logger, "added whitelist:%s", ss);
        }
        g_workerid = g_main_worker = ++(wtab->state->nworkers); 
        worker_running(g_workerid, port);
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
