#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include "common.h"
#define COM_PATH_MAX        256
/* mkdir force */
int force_mkdir(char *path)
{
    char fullpath[COM_PATH_MAX];
    int level = -1, ret = -1;
    struct stat st = {0};
    char *p = NULL;

    if(path)
    {
        strcpy(fullpath, path);
        p = fullpath;
        while(*p != '\0')
        {
            if(*p == '/' )
            {
                while(*p != '\0' && *p == '/' && *(p+1) == '/')++p;
                if(level > 0)
                {
                    *p = '\0';
                    memset(&st, 0, sizeof(struct stat));
                    ret = stat(fullpath, &st);
                    if(ret == 0 && !S_ISDIR(st.st_mode)) return -1;
                    if(ret != 0 && mkdir(fullpath, 0755) != 0) return -1;
                    *p = '/';
                }
                level++;
            }
            ++p;
        }
        return 0;
    }
    return -1;
}
/* set resource limit */
int setrlimiter(char *name, int rlimit, int nset)
{
    int ret = -1;
    struct rlimit rlim;
    if(name)
    {
        if(getrlimit(rlimit, &rlim) == -1)
            return -1;
        else
        {
            fprintf(stdout, "getrlimit %s cur[%ld] max[%ld]\n",
                    name, (long)rlim.rlim_cur, (long)rlim.rlim_max);
        }
        if(rlim.rlim_cur > nset && rlim.rlim_max > nset)
            return 0;
        rlim.rlim_cur = nset;
        rlim.rlim_max = nset;
        if((ret = setrlimit(rlimit, &rlim)) == 0)
        {
            fprintf(stdout, "setrlimit %s cur[%ld] max[%ld]\n",
                    name, (long)rlim.rlim_cur, (long)rlim.rlim_max);
            return 0;
        }
        else
        {
            fprintf(stderr, "setrlimit %s cur[%ld] max[%ld] failed, %s\n",
                    name, (long)rlim.rlim_cur, (long)rlim.rlim_max, strerror(errno));
        }
    }
    return ret;
}

int64_t strtotime64(char *strtime)
{
    struct tm tp = {0};
    int64_t times = 0;
    int msec = 0;
    if(sscanf(strtime, "%d-%d-%dT%d:%d:%d.%dZ", &(tp.tm_year), &(tp.tm_mon),
            &(tp.tm_mday),  &(tp.tm_hour), &(tp.tm_min), &(tp.tm_sec), &msec) == 7)
    {
        tp.tm_mon -= 1;
        if(tp.tm_year > 1900) tp.tm_year -= 1900;
        else if(tp.tm_year < 10) tp.tm_year += 100;
        times = ((int64_t)mktime(&tp) * 100000) + ((int64_t)msec * 1000);
    }
    return times;
}

int64_t nowtotime64()
{
    int64_t now = 0;
    struct timeval tv = {0};
    gettimeofday(&tv, NULL);now = (int64_t)tv.tv_sec * 1000000 + (int64_t)tv.tv_usec;
    return now;
}
