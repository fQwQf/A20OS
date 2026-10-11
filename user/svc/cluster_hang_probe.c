/* A yielding child whose only termination is the process-job deadline. */
#include "liba20rt/a20_sdk.h"
#include "liba20rt/crt0_a20.h"
int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;
    (void)envp;
    for (;;)
        a20_thread_yield();
}
