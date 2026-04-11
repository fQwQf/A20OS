/* mkdir — make directories */
#include "../lib/libc.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("mkdir: missing operand\n");
        return 1;
    }
    int status = 0;
    for (int i = 1; i < argc; i++) {
        if (mkdir(argv[i], 0755) < 0) {
            printf("mkdir: cannot create directory '%s'\n", argv[i]);
            status = 1;
        }
    }
    return status;
}
