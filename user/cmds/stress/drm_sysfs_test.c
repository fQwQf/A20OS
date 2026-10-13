#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define CHECK(x) do { if (!(x)) { printf("DRM_SYSFS: FAIL line=%d errno=%d %s\n", __LINE__, errno, #x); return 1; } } while (0)

static int read_text(const char *path, char *buf, size_t size) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t n = fread(buf, 1, size - 1, f);
    int error = ferror(f);
    fclose(f);
    buf[n] = 0;
    return error ? -1 : 0;
}

int main(void) {
    const char *names[] = {"card0", "renderD128"};
    char device[512] = "";
    for (unsigned i = 0; i < 2; i++) {
        char path[512], physical[512], by_class[512], text[512];
        struct stat st;
        snprintf(path, sizeof(path), "/sys/dev/char/226:%u", i ? 128 : 0);
        CHECK(lstat(path, &st) == 0 && S_ISLNK(st.st_mode));
        CHECK(realpath(path, physical));
        snprintf(path, sizeof(path), "/sys/class/drm/%s", names[i]);
        CHECK(realpath(path, by_class) && !strcmp(physical, by_class));
        snprintf(path, sizeof(path), "/sys/dev/char/226:%u/dev", i ? 128 : 0);
        CHECK(read_text(path, text, sizeof(text)) == 0);
        CHECK(!strcmp(text, i ? "226:128\n" : "226:0\n"));
        snprintf(path, sizeof(path), "/sys/dev/char/226:%u/uevent", i ? 128 : 0);
        CHECK(read_text(path, text, sizeof(text)) == 0);
        CHECK(strstr(text, i ? "DEVNAME=dri/renderD128\n" : "DEVNAME=dri/card0\n"));
        snprintf(path, sizeof(path), "/sys/dev/char/226:%u/device", i ? 128 : 0);
        CHECK(realpath(path, physical));
        if (!i) strcpy(device, physical);
        CHECK(!strcmp(device, physical));
    }
    char path[1024], text[512];
    snprintf(path, sizeof(path), "%s/drm", device);
    DIR *dir = opendir(path);
    CHECK(dir);
    unsigned found = 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, "card0")) found |= 1;
        if (!strcmp(entry->d_name, "renderD128")) found |= 2;
    }
    closedir(dir);
    CHECK(found == 3);
    snprintf(path, sizeof(path), "%s/uevent", device);
    CHECK(read_text(path, text, sizeof(text)) == 0);
    if (strstr(device, "/pci")) {
        CHECK(strstr(text, "PCI_SLOT_NAME="));
        CHECK(strstr(text, strrchr(device, '/') + 1));
        snprintf(path, sizeof(path), "%s/vendor", device);
        CHECK(read_text(path, text, sizeof(text)) == 0);
        unsigned vendor = strtoul(text, NULL, 16);
        snprintf(path, sizeof(path), "%s/config", device);
        FILE *f = fopen(path, "rb");
        CHECK(f);
        unsigned char config[64];
        CHECK(fread(config, 1, sizeof(config), f) == sizeof(config));
        fclose(f);
        CHECK(vendor == (unsigned)(config[0] | config[1] << 8));
    } else {
        CHECK(strstr(device, "/platform/"));
        CHECK(strstr(text, "MODALIAS=platform:"));
    }
    printf("DRM_SYSFS: PASS device=%s\n", device);
    return 0;
}
