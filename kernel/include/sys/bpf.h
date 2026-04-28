#ifndef _SYS_BPF_H
#define _SYS_BPF_H

int bpf_prog_is_loaded(int fd);
int bpf_run_socket_filter(int fd);

#endif
