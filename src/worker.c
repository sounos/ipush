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
#ifdef HAVE_SSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/crypto.h>
#endif
#include <pwd.h>
#include <signal.h>
#include <locale.h>
#include <pthread.h>
#include <errno.h>
#include <evbase.h>
#include "common.h"
#include "db.h"
#include "wtable.h"
#include "mtree.h"
#include "mqueue.h"
#include "logger.h"
#include "iniparser.h"
#include "xmm.h"
static WTABLE *wtab = NULL;
static dictionary *dict = NULL;
static int g_workerid = 0;
static int g_main_worker = 0;
static EVBASE *evbase = NULL;
static LOGGER *logger = NULL;
#define CONN_MAX    65536
#define CONN_BACKLOG_MAX 8192
#define EV_BUF_SIZE 65536
#define CONN_APP_MAX 32
typedef struct _CONN
{
    int status;
    int workerid;
    int keepalive;
    ushort port;
    ushort apps_num;
    int apps[CONN_APP_MAX];
    char ip[16];
#ifdef HAVE_SSL
    SSL *ssl;
#endif
    EVENT event;
}CONN;
static CONN *conns = NULL;
static int listenfd = 0;
static int port = 0;
static int is_use_SSL = 0;
#ifdef HAVE_SSL
static SSL_CTX *ctx = NULL;
#endif
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

void ev_ready_push()
{
    int wid = g_workerid, msgid = 0, appid = 0, mid = 0, conn_id = 0;
    WHEAD *head = NULL;

    while(mqueue_pop(wtab->queue, wtab->state->workers[wid].msg_qid, &msgid) > 0)
    {
        head = NULL;
        if(db_exists_block(wtab->mdb, msgid, (char **)&head) > sizeof(WHEAD)
                    && head && (appid = head->mix) > 0)
        {
            REALLOG(logger, "workers[%d] ready push app:%d msg:%d qtotal:%p/%d", wid, appid, msgid, wtab->state->workers[wid].map, mtree_total(wtab->state->workers[wid].map, appid));
            conn_id = 0;
            mid = mtree_max(wtab->state->workers[wid].map, appid, &conn_id, NULL);
            while(mid > 0 && conn_id > 0)
            {
                mqueue_push(wtab->state->workers[wid].queue,wtab->state->workers[wid].q[conn_id],msgid);
                REALLOG(logger, "workers[%d] ready push app:%d msg:%d to conn[%d] total:%d", wid, appid, msgid, conn_id, mtree_total(wtab->state->workers[wid].map, appid));
                event_add(&(conns[conn_id].event), E_WRITE);
                conn_id = 0;
                mid = mtree_prev(wtab->state->workers[wid].map, appid, (unsigned int)mid, &conn_id, NULL);
            }
        }
    }
    return ;
}

void ev_handler(int fd, int ev_flags, void *arg)
{
    struct  sockaddr_in rsa;
    socklen_t rsa_len = sizeof(struct sockaddr_in);
    char line[EV_BUF_SIZE], *p = NULL, *s = NULL, *ss = NULL, *xs = NULL, *msg = NULL;
    int rfd = 0, n = 0, appid = 0, msgid = 0, len = 0, i = 0;
    int64_t time = 0;

    if(fd == listenfd)
    {
        if((ev_flags & E_READ))
        {
            while((rfd = accept(fd, (struct sockaddr *)&rsa, &rsa_len)) > 0)
            {
                memset(&(conns[rfd]), 0, sizeof(CONN));
                strcpy(conns[rfd].ip, inet_ntoa(rsa.sin_addr));
                conns[rfd].port = ntohs(rsa.sin_port);
                if(is_use_SSL)
                {
#ifdef HAVE_SSL
                    if((conns[rfd].ssl = SSL_new(ctx)) == NULL)
                    {
                        FATAL_LOGGER(logger, "SSL_new() failed, %s", (char *)ERR_reason_error_string(ERR_get_error()));
                        shutdown(rfd, SHUT_RDWR);
                        close(rfd);
                        return ;
                    }
                    if(SSL_set_fd(conns[rfd].ssl, rfd) == 0)
                    {
                        ERR_print_errors_fp(stdout);
                        shutdown(rfd, SHUT_RDWR);
                        close(rfd);
                        return ;
                    }
                    if((SSL_accept(conns[rfd].ssl)) <= 0)
                    {
                        FATAL_LOGGER(logger, "SSL_Accept connection %s:%d via %d failed, %s",
                                inet_ntoa(rsa.sin_addr), ntohs(rsa.sin_port),
                                rfd,  ERR_reason_error_string(ERR_get_error()));
                        shutdown(rfd, SHUT_RDWR);
                        close(rfd);
                        return ;
                    }
#endif
                }
                REALLOG(logger, "worker[%d] new connection[%s:%d] via %d", g_workerid, conns[rfd].ip, conns[rfd].port, rfd)
                event_set(&(conns[rfd].event), rfd, E_READ|E_PERSIST,
                        (void *)&(conns[rfd].event), &ev_handler);
                evbase->add(evbase, &(conns[rfd].event));
                ++(wtab->state->conn_total);
                wtable_newconn(wtab, g_workerid, rfd);
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
            if(is_use_SSL)
            {
#ifdef HAVE_SSL
                n = SSL_read(conns[fd].ssl, line, EV_BUF_SIZE);
#endif
            }
            else
            {
                n = read(fd, line, EV_BUF_SIZE);
            }
            if(n <= 0) goto err;
            line[n] = 0;
            ss = s = line;
            REALLOG(logger, "read %d bytes from %s:%d via %d", n, conns[fd].ip, conns[fd].port, fd);
            while((p = strchr(ss, '\n')))
            {
                REALLOG(logger, "%.*s conn[%s:%d] via %d", p - ss - 1, ss, conns[fd].ip, conns[fd].port, fd);
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
                            REALLOG(logger, "new appkey[%s] from conn[%s:%d] via %d", xs, conns[fd].ip, conns[fd].port, fd)
                            *s = '"';
                        }
                    }
                    else 
                    {
                        REALLOG(logger, "WARN!!! new-app but not whitelist conn[%s:%d] via %d", conns[fd].ip, conns[fd].port, fd)
                        goto err;
                    }
                }
                else if(strncmp(s, "{\"time\":\"", 9) == 0)
                {
                    s += 9;
                    time = nowtotime64();
                    xs = s;
                    while(*s != '\0' && *s != '"') ++s;
                    if(*s == '"')
                    {
                        *s = '\0';
                        time = strtotime64(xs);
                        *s = '"';
                    }
                    if((s = strstr(s, "\"oauth_key\":\"")) && wtable_check_whitelist(wtab, inet_addr(conns[fd].ip)) > 0)
                    {/* check whitelist & add msg */
                        s += 13;
                        xs = s;
                        while(*s != '\0' && *s != '"') ++s;
                        if(*s == '"')
                        {
                            *s = '\0';
                            appid = wtable_appid(wtab, xs, s - xs);
                            *s = '"';
                            n = (p+1) - ss;
                            msgid = wtable_new_msg(wtab, appid, ss, n, time);
                            REALLOG(logger, "new-msg[%.*s] for app:[%d:%.*s] from conn[%s:%d] via %d", n - 1 , ss, appid, s - xs, xs, conns[fd].ip, conns[fd].port, fd)
                        }
                    }
                    else 
                    {
                        REALLOG(logger, "WARN!!! new-msg but not whitelist conn[%s:%d] via %d", conns[fd].ip, conns[fd].port, fd)
                            goto err;
                    }
                }
                else if(strncmp(s, "{\"last\":", 8) == 0)
                {
                    s += 8;
                    time = nowtotime64();
                    if(*s == '"')
                    {
                        xs = ++s;
                        while(*s != '\0' && *s != '"') ++s;
                        if(*s == '"')
                        {
                            *s = '\0';
                            time = strtotime64(xs);
                            *s = '"';
                        }
                    }
                    if((s = strstr(s, "\"oauth_key\":\"")))
                    {/* auth key  */
                        s += 13;
                        xs = s;
                        while(*s != '\0' && *s != '"') ++s;
                        if(*s == '"')
                        {
                            *s = '\0';
                            if((appid=wtable_app_auth(wtab, g_workerid, xs, s - xs,fd, time))< 1) 
                            {
                                REALLOG(logger, "WARN!!! unknown appkey[%.*s] from conn[%s:%d] via %d", s - xs, xs, conns[fd].ip, conns[fd].port, fd)
                                goto err;
                            }
                            if((i = conns[fd].apps_num) < CONN_APP_MAX)
                            {
                                conns[fd].apps[i] = appid;
                                conns[fd].apps_num++;
                            }
                            event_add(&(conns[fd].event), E_WRITE);
                            *s = '"';
                        }
                    }
                    else 
                    {
                        REALLOG(logger, "WARN!!! bad request[%s] from conn[%s:%d] via %d", line, conns[fd].ip, conns[fd].port, fd)
                        goto err;
                    }
                }
                else goto err;
                ss = s = ++p;
            }
        }
        if(ev_flags & E_WRITE)
        {
            msg = NULL;
            if((len = wtable_get_msg(wtab, g_workerid, fd, &msg)) > 0 && msg) 
            {
                if(is_use_SSL)
                {
#ifdef HAVE_SSL
                    n = SSL_write(conns[fd].ssl, msg, len);
#endif
                }
                else
                {
                    n = write(fd, msg, len);
                }
                REALLOG(logger, "workers[%d] sent msg{%.*s} to %s:%d", g_workerid, len - 1, msg, conns[fd].ip, conns[fd].port);
                if(n <= 0) goto err;
                if(wtable_over_msg(wtab, g_workerid, fd) < 1)
                {
                    event_del(&(conns[fd].event), E_WRITE);
                }
            }
            else 
            {
                event_del(&(conns[fd].event), E_WRITE);
            }
        }
        return ;
err:
        event_destroy(&(conns[fd].event));
        wtable_endconn(wtab, g_workerid, fd, conns[fd].apps, conns[fd].apps_num);
#ifdef HAVE_SSL
        if(conns[fd].ssl)
        {   SSL_shutdown(conns[fd].ssl);
            SSL_free(conns[fd].ssl);
            conns[fd].ssl = NULL;
        }
#endif
        memset(&(conns[fd]), 0, sizeof(CONN));
        shutdown(fd, SHUT_RDWR);
        close(fd);
        --(wtab->state->conn_total);
        //REALLOG(logger, "conn_total:%d", wtab->state->conn_total);
    }
    return ;
}

/* main worker running */
void worker_running(int wid, int listenport)
{
    WORKER *workers = wtab->state->workers;
    int opt = 1, taskid = 0;
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
            while((taskid = wtable_pop_task(wtab, g_workerid)) > 0)
            {
                if(taskid == W_CMD_STOP) goto stop;
            }
            ev_ready_push();
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
    int wid = (int)((long)arg), taskid = 0;
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
#ifdef RUN_TEST
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
    
#ifdef HAVE_SSL
    if(is_use_SSL)
    {
        SSL_library_init();
        OpenSSL_add_all_algorithms();
        SSL_load_error_strings();
        if((ctx = SSL_CTX_new(SSLv23_server_method())) == NULL)
        {
            ERR_print_errors_fp(stdout);
            exit(1);
        }
        /*load certificate */
        if (SSL_CTX_use_certificate_file(ctx, cert, SSL_FILETYPE_PEM) <= 0)
        {
            ERR_print_errors_fp(stdout);
            exit(1);
        }
        /*load private key file */
        if (SSL_CTX_use_PrivateKey_file(ctx, privkey, SSL_FILETYPE_PEM) <= 0)
        {
            ERR_print_errors_fp(stdout);
            exit(1);
        }
        /*check private key file */
        if (!SSL_CTX_check_private_key(ctx))
        {
            ERR_print_errors_fp(stdout);
            exit(1);
        }
    }
#endif
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
#else
int main(int argc, char **argv)
{
    char log[256], *ss = NULL, *s = NULL, *workdir = NULL, *whitelist = NULL, 
         *conf = NULL, ch = 0, *p = NULL;
    struct passwd *user = NULL;
    int is_run_daemon = 0;

    setrlimiter("RLIMIT_NOFILE", RLIMIT_NOFILE, CONN_MAX);
    /* get configure file */
    while((ch = getopt(argc, argv, "c:d")) != (char)-1)
    {
        if(ch == 'c') conf = optarg;
        else if(ch == 'd') is_run_daemon = 1;
    }
    if(conf == NULL)
    {
        fprintf(stderr, "Usage:%s -d -c config_file\n", argv[0]);
        _exit(-1);
    }
    if((dict = iniparser_new(conf)) == NULL)
    {
        fprintf(stderr, "Initializing conf:%s failed, %s\n", conf, strerror(errno));
        _exit(-1);
    }
    p = iniparser_getstr(dict, "IPUSHD:user");
    if((user = getpwnam((const char *)p)) == NULL || setuid(user->pw_uid)) 
    {
        fprintf(stderr, "setuid(%s) for ipushd failed, %s\n", p, strerror(errno));
        exit(EXIT_FAILURE);
    }
    if((port = iniparser_getint(dict, "IPUSHD:port", 0)) == 0)
    {
        fprintf(stderr, "invalid listen port:%d\n", port);
        exit(EXIT_FAILURE);
    }
    if(!(workdir = iniparser_getstr(dict, "IPUSHD:workdir")))
    {
        fprintf(stderr, "NULL workdir path\n");
        exit(EXIT_FAILURE);
    }
    if(!(whitelist = iniparser_getstr(dict, "IPUSHD:whitelist")))
    {
        fprintf(stderr, "NULL whitelist\n");
        exit(EXIT_FAILURE);
    }
    is_use_SSL = iniparser_getint(dict, "IPUSHD:is_use_SSL", 0);
    cert = iniparser_getstr(dict, "IPUSHD:cacert_file");
    privkey = iniparser_getstr(dict, "IPUSHD:privkey_file");
    if(is_use_SSL && (!cert || !privkey || access(cert, F_OK) != 0 || access(privkey, F_OK) != 0))
    {
        fprintf(stderr, "certfile OR privkey must be exists\n");
        exit(EXIT_FAILURE);
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
    conns = (CONN *)xmm_mnew(sizeof(CONN) * CONN_MAX);
    if(conns == NULL) exit(EXIT_FAILURE);
#ifdef HAVE_SSL
    if(is_use_SSL)
    {
        SSL_library_init();
        OpenSSL_add_all_algorithms();
        SSL_load_error_strings();
        if((ctx = SSL_CTX_new(SSLv23_server_method())) == NULL)
        {
            ERR_print_errors_fp(stdout);
            exit(1);
        }
        /*load certificate */
        if (SSL_CTX_use_certificate_file(ctx, cert, SSL_FILETYPE_PEM) <= 0)
        {
            ERR_print_errors_fp(stdout);
            exit(1);
        }
        /*load private key file */
        if (SSL_CTX_use_PrivateKey_file(ctx, privkey, SSL_FILETYPE_PEM) <= 0)
        {
            ERR_print_errors_fp(stdout);
            exit(1);
        }
        /*check private key file */
        if (!SSL_CTX_check_private_key(ctx))
        {
            ERR_print_errors_fp(stdout);
            exit(1);
        }
    }
#endif
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
    if(dict)iniparser_free(dict);
    return 0;
}
#endif
