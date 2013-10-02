#ifndef __COMMON__H__
#define __COMMON__H__
/* mkdir force */
int force_mkdir(char *path);
/* set resource limit */
int setrlimiter(char *name, int rlimit, int nset);
#endif
