#ifndef A20_NET_LANE_HOST_CPU_H
#define A20_NET_LANE_HOST_CPU_H
extern __thread unsigned a20_host_cpu;
static inline unsigned cpu_current_id(void) {
    return a20_host_cpu;
}
#endif
