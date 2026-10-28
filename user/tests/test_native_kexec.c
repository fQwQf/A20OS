#include "liba20rt/a20_sdk.h"
#include "liba20rt/a20_kexec.h"
#include "liba20c/include/fcntl.h"
#include "liba20c/include/stdio.h"
#include "liba20c/include/stdlib.h"
#include "liba20c/include/unistd.h"
#include "liba20c/include/sys/stat.h"

int main(int argc, char **argv, char **envp)
{
    (void)argc;
    (void)argv;
    (void)envp;
    const char *path = "/bin/boot/kernel-next.elf";
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("NATIVE_KEXEC: FAIL open %s\n", path);
        return 1;
    }
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size <= 0 ||
        (uint64_t)st.st_size > 64 * 1024 * 1024) {
        printf("NATIVE_KEXEC: FAIL size\n");
        close(fd);
        return 1;
    }
    size_t size = (size_t)st.st_size;
    void *image = malloc(size);
    if (!image) {
        close(fd);
        return 1;
    }
    size_t done = 0;
    while (done < size) {
        ssize_t got = read(fd, (char *)image + done, size - done);
        if (got <= 0) {
            printf("NATIVE_KEXEC: FAIL read\n");
            free(image);
            close(fd);
            return 1;
        }
        done += (size_t)got;
    }
    close(fd);
    a20_status_t rc = a20_kexec_load_image(image, size);
    free(image);
    if (rc != A20_OK) {
        printf("NATIVE_KEXEC: FAIL load %lld\n", (long long)rc);
        return 1;
    }
    a20_kexec_status_t status;
    rc = a20_kexec_status(&status);
    if (rc != A20_OK || status.state != A20_KEXEC_IMAGE_LOADED ||
        !(status.backend & A20_KEXEC_BACKEND_RISCV64_QEMU)) {
        printf("NATIVE_KEXEC: FAIL status %lld state=%u backend=%x\n",
               (long long)rc, status.state, status.backend);
        return 1;
    }
    printf("NATIVE_KEXEC: LOADED backend=1 segments=%u\n", status.nr_segments);
    fflush(stdout);
    rc = a20_kexec_execute();
    printf("NATIVE_KEXEC: FAIL execute %lld\n", (long long)rc);
    return 1;
}
