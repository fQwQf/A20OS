/*
 * drvctl - stage a signed A20 kernel-driver package in DriverStore.
 *
 * This command deliberately does not load native code by itself. It verifies
 * the small line-oriented manifest, copies the module and manifest to the
 * persistent driver store, and leaves activation to the kernel boot/driver
 * manager. Runtime activation will be added with the native driver-control
 * ABI once device removal and process-wide page-table invalidation are wired.
 */

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define DRIVER_STORE "/bin/lib/drivers/"
#define MANIFEST_MAX 4096
#define NAME_MAX_LEN 48

static int valid_name(const char *name)
{
    size_t n = strlen(name);
    if (n == 0 || n >= NAME_MAX_LEN)
        return 0;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_' || c == '.'))
            return 0;
    }
    return 1;
}

static int read_manifest(const char *path, char *buf, size_t cap)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, cap - 1);
    close(fd);
    if (n <= 0 || (size_t)n >= cap - 1)
        return -1;
    buf[n] = '\0';
    return 0;
}

static int has_key(const char *manifest, const char *key)
{
    size_t n = strlen(key);
    const char *p = manifest;
    while ((p = strstr(p, key)) != NULL) {
        if ((p == manifest || p[-1] == '\n') && p[n] != '\0')
            return 1;
        p += n;
    }
    return 0;
}

static int copy_file(const char *src, const char *dst)
{
    int in = open(src, O_RDONLY);
    if (in < 0)
        return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        close(in);
        return -1;
    }

    char buf[4096];
    int result = 0;
    for (;;) {
        ssize_t n = read(in, buf, sizeof(buf));
        if (n == 0)
            break;
        if (n < 0 || write(out, buf, (size_t)n) != n) {
            result = -1;
            break;
        }
    }
    fsync(out);
    close(out);
    close(in);
    if (result < 0)
        unlink(dst);
    return result;
}

static int store_path(char *out, size_t cap, const char *name,
                      const char *suffix)
{
    int n = snprintf(out, cap, "%s%s%s", DRIVER_STORE, name, suffix);
    return n > 0 && (size_t)n < cap ? 0 : -1;
}

static int install_package(const char *module, const char *manifest,
                           const char *name)
{
    char contents[MANIFEST_MAX];
    char module_dst[128];
    char manifest_dst[128];
    if (!valid_name(name) || read_manifest(manifest, contents, sizeof(contents)) < 0)
        return 1;
    if (!has_key(contents, "name=") || !has_key(contents, "module=") ||
        !has_key(contents, "bus=") || !has_key(contents, "match="))
        return 2;
    if (store_path(module_dst, sizeof(module_dst), name, ".drv") < 0 ||
        store_path(manifest_dst, sizeof(manifest_dst), name, ".a20inf") < 0)
        return 3;
    if (copy_file(module, module_dst) < 0)
        return 4;
    if (copy_file(manifest, manifest_dst) < 0) {
        unlink(module_dst);
        return 5;
    }
    printf("DRVCTL: staged %s and %s\n", module_dst, manifest_dst);
    printf("DRVCTL: activation pending next driver-manager pass\n");
    return 0;
}

static int remove_package(const char *name)
{
    char module[128];
    char manifest[128];
    if (!valid_name(name) || store_path(module, sizeof(module), name, ".drv") < 0 ||
        store_path(manifest, sizeof(manifest), name, ".a20inf") < 0)
        return 1;
    int result = 0;
    if (unlink(module) < 0)
        result = 1;
    if (unlink(manifest) < 0)
        result = 1;
    return result;
}

static int list_packages(void)
{
    DIR *dir = opendir(DRIVER_STORE);
    if (!dir)
        return 1;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        size_t n = strlen(entry->d_name);
        if (n > 4 && strcmp(entry->d_name + n - 4, ".drv") == 0)
            puts(entry->d_name);
    }
    closedir(dir);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "list") == 0)
        return list_packages();
    if (argc == 3 && strcmp(argv[1], "remove") == 0)
        return remove_package(argv[2]);
    if (argc == 5 && strcmp(argv[1], "install") == 0)
        return install_package(argv[2], argv[3], argv[4]);

    fprintf(stderr, "usage: drvctl install MODULE MANIFEST NAME\n");
    fprintf(stderr, "       drvctl remove NAME\n");
    fprintf(stderr, "       drvctl list\n");
    return 2;
}
