/* Deterministic child for real Native process-job acceptance. */
#include "liba20rt/crt0_a20.h"
int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;
    (void)envp;
    return 42;
}
