#ifndef _ABI_LINUX_RESOURCE_H
#define _ABI_LINUX_RESOURCE_H

#define PR_SET_NAME          15
#define PR_CAPBSET_READ      23
#define PR_CAPBSET_DROP      24
#define PR_SET_THP_DISABLE   41
#define PR_GET_THP_DISABLE   42

#define RLIMIT_STACK   3
#define RLIMIT_NOFILE  7
#define RLIM_NLIMITS   16

#define RUSAGE_SELF      0
#define RUSAGE_CHILDREN -1
#define RUSAGE_THREAD    1

#endif /* _ABI_LINUX_RESOURCE_H */
