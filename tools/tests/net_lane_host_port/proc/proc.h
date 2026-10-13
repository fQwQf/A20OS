#ifndef A20_NET_LANE_HOST_PROC_H
#define A20_NET_LANE_HOST_PROC_H
/* The TLS address models task identity independently of the simulated CPU. */
typedef struct a20_host_task task_t;
extern __thread unsigned a20_host_cpu;
static inline task_t *proc_current(void) {
    return (task_t *)&a20_host_cpu;
}
#endif
