/*
 * drvctl - manage A20 driver packages (.a20drv) in the DriverStore.
 *
 * The .a20drv descriptor inside the package is the ONLY driver metadata
 * (there is no sidecar manifest).  drvctl validates the descriptor, stages
 * the package into the persistent DriverStore, and leaves activation to
 * the kernel driver manager on the next boot (driver_manager_init).  The
 * same descriptor is the metadata source for the kernel manager and for
 * `drvctl list`.
 */

#include <dirent.h>
#include <elf.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define DRIVER_STORE "/bin/lib/drivers/"
#define NAME_MAX_LEN 48

#define A20_DRIVER_DESCRIPTOR_MAGIC   0x41323044U /* "A20D" */
#define A20_DRIVER_DESCRIPTOR_VERSION 2U
#define A20_DRIVER_MAX_MATCH 4

/* Byte layout mirrors kernel/include/drivers/driver_descriptor.h
 * (a20_driver_descriptor_t).  All fields are 4-byte aligned, so the C
 * layout is identical to the kernel's. */
typedef struct a20_driver_match_desc {
    uint32_t bus;
    uint32_t vendor;
    uint32_t device;
} a20_driver_match_desc_t;

typedef struct a20_driver_desc {
    uint32_t magic;
    uint32_t version;
    uint32_t placement;
    uint32_t type;
    char name[32];
    uint32_t abi;
    uint32_t resource_mask;
    uint32_t reserved;
    a20_driver_match_desc_t match[A20_DRIVER_MAX_MATCH];
    uint32_t match_count;
} a20_driver_desc_t;

_Static_assert(sizeof(a20_driver_desc_t) == 112, "descriptor layout drift");

static const char *placement_str(uint32_t p)
{
    switch (p) {
    case 1: return "kernel-module";
    case 2: return "user-service";
    default: return "?";
    }
}

static const char *type_str(uint32_t t)
{
    switch (t) {
    case 1: return "rtc";
    case 2: return "block";
    case 3: return "input";
    case 4: return "audio";
    case 5: return "security";
    case 6: return "net";
    case 7: return "display";
    case 8: return "usb";
    default: return "?";
    }
}

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

static int read_descriptor(const char *path, a20_driver_desc_t *out)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;

    Elf64_Ehdr eh;
    if (read(fd, &eh, sizeof(eh)) != (ssize_t)sizeof(eh)) {
        close(fd);
        return -1;
    }
    if (memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 ||
        eh.e_ident[EI_CLASS] != ELFCLASS64 ||
        eh.e_shentsize != sizeof(Elf64_Shdr) || eh.e_shnum == 0 ||
        eh.e_shnum > 128 || eh.e_shstrndx >= eh.e_shnum) {
        close(fd);
        return -1;
    }

    Elf64_Shdr shdrs[128];
    if (pread(fd, shdrs, sizeof(Elf64_Shdr) * eh.e_shnum, eh.e_shoff) !=
        (ssize_t)(sizeof(Elf64_Shdr) * eh.e_shnum)) {
        close(fd);
        return -1;
    }
    Elf64_Shdr *shstr = &shdrs[eh.e_shstrndx];
    if (shstr->sh_size == 0 || shstr->sh_size > 4096) {
        close(fd);
        return -1;
    }
    char names[4096];
    if (pread(fd, names, shstr->sh_size, shstr->sh_offset) !=
        (ssize_t)shstr->sh_size) {
        close(fd);
        return -1;
    }

    for (int i = 0; i < eh.e_shnum; i++) {
        Elf64_Shdr *sh = &shdrs[i];
        if (sh->sh_name >= shstr->sh_size)
            continue;
        if (strcmp(names + sh->sh_name, ".a20drv") != 0)
            continue;
        if (sh->sh_type != SHT_PROGBITS || sh->sh_size != sizeof(*out)) {
            close(fd);
            return -1;
        }
        if (pread(fd, out, sizeof(*out), sh->sh_offset) !=
            (ssize_t)sizeof(*out)) {
            close(fd);
            return -1;
        }
        close(fd);
        if (out->magic != A20_DRIVER_DESCRIPTOR_MAGIC ||
            out->version != A20_DRIVER_DESCRIPTOR_VERSION ||
            out->match_count > A20_DRIVER_MAX_MATCH || !out->name[0])
            return -1;
        return 0;
    }
    close(fd);
    return -1;
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

static int store_path(char *out, size_t cap, const char *name)
{
    int n = snprintf(out, cap, "%s%s.a20drv", DRIVER_STORE, name);
    return n > 0 && (size_t)n < cap ? 0 : -1;
}

static int install_package(const char *module, const char *name)
{
    char dst[128];
    a20_driver_desc_t desc;
    if (!valid_name(name))
        return 1;
    if (read_descriptor(module, &desc) < 0) {
        fprintf(stderr, "drvctl: %s has no valid .a20drv descriptor\n",
                module);
        return 2;
    }
    if (store_path(dst, sizeof(dst), name) < 0)
        return 3;
    if (copy_file(module, dst) < 0)
        return 4;
    printf("DRVCTL: staged %s (placement=%s type=%s)\n", dst,
           placement_str(desc.placement), type_str(desc.type));
    printf("DRVCTL: activation pending next driver-manager pass\n");
    return 0;
}

static int remove_package(const char *name)
{
    char dst[128];
    if (!valid_name(name) || store_path(dst, sizeof(dst), name) < 0)
        return 1;
    int result = unlink(dst) < 0 ? 1 : 0;
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
        if (n <= 7 || strcmp(entry->d_name + n - 7, ".a20drv") != 0)
            continue;
        char path[128];
        snprintf(path, sizeof(path), "%s%s", DRIVER_STORE, entry->d_name);
        a20_driver_desc_t desc;
        if (read_descriptor(path, &desc) == 0)
            printf("%-40s placement=%s type=%s match=%u\n",
                   entry->d_name, placement_str(desc.placement),
                   type_str(desc.type), desc.match_count);
        else
            printf("%-40s <invalid descriptor>\n", entry->d_name);
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
    if (argc == 4 && strcmp(argv[1], "install") == 0)
        return install_package(argv[2], argv[3]);

    fprintf(stderr, "usage: drvctl install MODULE NAME\n");
    fprintf(stderr, "       drvctl remove NAME\n");
    fprintf(stderr, "       drvctl list\n");
    return 2;
}
