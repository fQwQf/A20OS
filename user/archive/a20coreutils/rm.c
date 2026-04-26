/* rm — remove files or directories */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("rm: missing operand\n");
        return 1;
    }
    int status = 0;
    int recursive = 0;
    int force = 0;
    
    int arg_idx = 1;
    while (arg_idx < argc && argv[arg_idx][0] == '-') {
        for (int j = 1; argv[arg_idx][j]; j++) {
            if (argv[arg_idx][j] == 'r' || argv[arg_idx][j] == 'R') recursive = 1;
            if (argv[arg_idx][j] == 'f') force = 1;
        }
        arg_idx++;
    }

    if (arg_idx >= argc) {
        if (!force) {
            printf("rm: missing operand\n");
            return 1;
        }
        return 0;
    }

    for (int i = arg_idx; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st) < 0) {
            if (!force) {
                printf("rm: cannot remove '%s': No such file or directory\n", argv[i]);
                status = 1;
            }
            continue;
        }
        
        if (S_ISDIR(st.st_mode)) {
            if (!recursive) {
                printf("rm: cannot remove '%s': Is a directory\n", argv[i]);
                status = 1;
            } else {
                /* We would normally recurse here, for A20OS we just call rmdir and hope it's empty
                 * or we wait for a vfs recursive implementation */
                if (rmdir(argv[i]) < 0) {
                    if (!force) {
                        printf("rm: cannot remove directory '%s'\n", argv[i]);
                        status = 1;
                    }
                }
            }
        } else {
            if (unlink(argv[i]) < 0) {
                if (!force) {
                    printf("rm: cannot remove '%s'\n", argv[i]);
                    status = 1;
                }
            }
        }
    }
    return status;
}
