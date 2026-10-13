#ifndef _ABI_LINUX_RESOURCE_H
#define _ABI_LINUX_RESOURCE_H

#define PR_SET_PDEATHSIG      1
#define PR_GET_DUMPABLE       3
#define PR_SET_DUMPABLE       4
#define PR_GET_PDEATHSIG      2
#define PR_SET_NAME          15
#define PR_GET_SECCOMP       21
#define PR_SET_SECCOMP       22
#define PR_CAPBSET_READ      23
#define PR_CAPBSET_DROP      24
#define PR_SET_NO_NEW_PRIVS  38
#define PR_GET_NO_NEW_PRIVS  39
#define PR_SET_THP_DISABLE   41
#define PR_GET_THP_DISABLE   42

#define RLIMIT_STACK   3
#define RLIMIT_CORE    4
#define RLIMIT_NOFILE  7
#define RLIMIT_AS      9
#define RLIMIT_NPROC   6
#define RLIMIT_RTPRIO  14
#define RLIM_NLIMITS   16

#define RUSAGE_SELF      0
#define RUSAGE_CHILDREN -1
#define RUSAGE_THREAD    1

#endif /* _ABI_LINUX_RESOURCE_H */
