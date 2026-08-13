/* libudev stub for A20OS, backed by the real /sys tree.
 *
 * A full udev/eudev daemon is out of scope for this image; this library
 * provides the enumeration surface that libinput's udev backend and wlroots'
 * DRM discovery need.  Devices are discovered by scanning the class
 * directories below /sys/class instead of being hardcoded, so new input or
 * display devices registered by drivers show up automatically.
 *
 * Properties (ID_INPUT / ID_SEAT / ...) that a real udev derives from
 * hwdb + uevent are synthesized here: A20OS sysfs does not yet publish a
 * per-device uevent file for every class device.  Hotplug is still not
 * supported. */

#include "udev.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_udev_dummy;

struct stub_udev_device {
    int refs;
    char syspath[256];
    char sysname[64];
    char subsystem[32];
    char devnode[128];
};

struct stub_udev_monitor {
    int refs;
    int fd;
};

struct stub_udev_enumerate {
    int refs;
    int graphics; /* display subsystem requested */
    int input;    /* input subsystem requested */
    int drm;      /* drm subsystem requested */
    struct stub_udev_list_entry *entries;
};

struct stub_udev_list_entry {
    struct stub_udev_list_entry *next;
    char name[256];
    char value[256];
};

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

/* Return the trailing component of a slash-terminated or slash-free path,
 * or the empty string if the path ends in '/'. */
static void path_split(const char *path, char *parent_out, size_t parent_len,
                       char *leaf_out, size_t leaf_len)
{
    const char *slash = strrchr(path, '/');
    if (slash == NULL) {
        if (leaf_out)
            snprintf(leaf_out, leaf_len, "%s", path);
        if (parent_out)
            parent_out[0] = '\0';
        return;
    }
    /* sysname is the last component (non-empty) */
    if (leaf_out) {
        const char *leaf = slash + 1;
        if (*leaf == '\0') {
            /* strip trailing slash, retry on the prefix */
            char tmp[256];
            size_t n = (size_t)(slash - path);
            if (n >= sizeof(tmp))
                n = sizeof(tmp) - 1;
            memcpy(tmp, path, n);
            tmp[n] = '\0';
            path_split(tmp, parent_out, parent_len, leaf_out, leaf_len);
            return;
        }
        snprintf(leaf_out, leaf_len, "%s", leaf);
    }
    if (parent_out) {
        size_t n = (size_t)(slash - path);
        if (n >= parent_len)
            n = parent_len - 1;
        memcpy(parent_out, path, n);
        parent_out[n] = '\0';
    }
}

static const char *subsystem_of_syspath(const char *syspath, char *out, size_t len)
{
    char leaf[64], parent[256];
    path_split(syspath, parent, sizeof(parent), leaf, sizeof(leaf));
    /* parent ends in ".../class/<subsystem>", so its leaf is the subsystem */
    path_split(parent, NULL, 0, out ? out : leaf, len);
    if (out == NULL)
        return NULL;
    return out;
}

static const char *devnode_for(const char *subsystem, const char *sysname,
                               char *out, size_t len)
{
    if (strcmp(subsystem, "input") == 0)
        snprintf(out, len, "/dev/input/%s", sysname);
    else if (strcmp(subsystem, "display") == 0 || strcmp(subsystem, "graphics") == 0)
        snprintf(out, len, "/dev/%s", sysname);
    else if (strcmp(subsystem, "drm") == 0)
        snprintf(out, len, "/dev/dri/%s", sysname);
    else
        snprintf(out, len, "/dev/%s", sysname);
    return out;
}

static int is_input_subsystem(const char *subsystem)
{
    return strcmp(subsystem, "input") == 0;
}

/* ------------------------------------------------------------------ */
/* udev_device                                                         */
/* ------------------------------------------------------------------ */

static struct stub_udev_device *stub_device_new(const char *syspath)
{
    struct stub_udev_device *d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->refs = 1;

    if (syspath == NULL || *syspath == '\0')
        syspath = "/sys/class/input/event0";

    snprintf(d->syspath, sizeof(d->syspath), "%s", syspath);

    char leaf[64];
    path_split(syspath, NULL, 0, leaf, sizeof(leaf));
    snprintf(d->sysname, sizeof(d->sysname), "%s", leaf);
    subsystem_of_syspath(syspath, d->subsystem, sizeof(d->subsystem));
    devnode_for(d->subsystem, d->sysname, d->devnode, sizeof(d->devnode));
    return d;
}

struct udev *udev_new(void) { return (struct udev *)&g_udev_dummy; }
struct udev *udev_ref(struct udev *udev) { return udev; }
struct udev *udev_unref(struct udev *udev) { (void)udev; return NULL; }
int udev_get_log_priority(struct udev *udev) { (void)udev; return 0; }
void udev_set_log_priority(struct udev *udev, int priority) { (void)udev; (void)priority; }
int udev_get_userdata(struct udev *udev, void **userdata) { (void)udev; (void)userdata; return 0; }
void udev_set_userdata(struct udev *udev, void *userdata) { (void)udev; (void)userdata; }
struct udev_list_entry *udev_get_properties_list_entry(struct udev *udev) { (void)udev; return NULL; }

struct udev_device *udev_device_new_from_syspath(struct udev *udev, const char *syspath) {
    (void)udev;
    return (struct udev_device *)stub_device_new(syspath);
}
struct udev_device *udev_device_new_from_devnum(struct udev *udev, char type, dev_t devnum) { (void)udev; (void)type; (void)devnum; return (struct udev_device *)stub_device_new(NULL); }
struct udev_device *udev_device_new_from_subsystem_sysname(struct udev *udev, const char *subsystem, const char *sysname) {
    (void)udev;
    char path[256];
    if (subsystem == NULL || sysname == NULL)
        return NULL;
    snprintf(path, sizeof(path), "/sys/class/%s/%s", subsystem, sysname);
    return (struct udev_device *)stub_device_new(path);
}
struct udev_device *udev_device_new_from_device_id(struct udev *udev, const char *id) { (void)udev; (void)id; return NULL; }
struct udev_device *udev_device_new_from_environment(struct udev *udev) { (void)udev; return NULL; }
struct udev_device *udev_device_ref(struct udev_device *udev_device) {
    struct stub_udev_device *d = (struct stub_udev_device *)udev_device;
    if (d) d->refs++;
    return udev_device;
}
struct udev_device *udev_device_unref(struct udev_device *udev_device) {
    struct stub_udev_device *d = (struct stub_udev_device *)udev_device;
    if (d && --d->refs <= 0)
        free(d);
    return NULL;
}
struct udev *udev_device_get_udev(struct udev_device *udev_device) { (void)udev_device; return (struct udev *)&g_udev_dummy; }
struct udev_device *udev_device_get_parent(struct udev_device *udev_device) { (void)udev_device; return NULL; }
struct udev_device *udev_device_get_parent_with_subsystem_devtype(struct udev_device *udev_device, const char *subsystem, const char *devtype) { (void)udev_device; (void)subsystem; (void)devtype; return NULL; }
const char *udev_device_get_devpath(struct udev_device *udev_device) {
    struct stub_udev_device *d = (struct stub_udev_device *)udev_device;
    return d ? d->syspath : NULL;
}
const char *udev_device_get_subsystem(struct udev_device *udev_device) {
    struct stub_udev_device *d = (struct stub_udev_device *)udev_device;
    return d ? d->subsystem : NULL;
}
const char *udev_device_get_devtype(struct udev_device *udev_device) { (void)udev_device; return NULL; }
const char *udev_device_get_syspath(struct udev_device *udev_device) {
    struct stub_udev_device *d = (struct stub_udev_device *)udev_device;
    return d ? d->syspath : NULL;
}
const char *udev_device_get_sysname(struct udev_device *udev_device) {
    struct stub_udev_device *d = (struct stub_udev_device *)udev_device;
    return d ? d->sysname : NULL;
}
const char *udev_device_get_sysnum(struct udev_device *udev_device) {
    struct stub_udev_device *d = (struct stub_udev_device *)udev_device;
    if (!d)
        return NULL;
    const char *p = d->sysname;
    while (*p && !(*p >= '0' && *p <= '9'))
        p++;
    return *p ? p : "0";
}
const char *udev_device_get_devnode(struct udev_device *udev_device) {
    struct stub_udev_device *d = (struct stub_udev_device *)udev_device;
    return d ? d->devnode : NULL;
}
dev_t udev_device_get_devnum(struct udev_device *udev_device) {
    struct stub_udev_device *d = (struct stub_udev_device *)udev_device;
    if (!d)
        return 0;
    char path[320];
    snprintf(path, sizeof(path), "%s/dev", d->syspath);
    char buf[32];
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = '\0';
    unsigned maj = 0, min = 0;
    if (sscanf(buf, "%u:%u", &maj, &min) != 2)
        return 0;
    return (dev_t)(((unsigned long long)maj << 20) | min); /* mkdev-ish, unused by libinput */
}
const char *udev_device_get_driver(struct udev_device *udev_device) { (void)udev_device; return NULL; }
const char *udev_device_get_action(struct udev_device *udev_device) { (void)udev_device; return NULL; }
unsigned long long int udev_device_get_seqnum(struct udev_device *udev_device) { (void)udev_device; return 0; }
unsigned long long int udev_device_get_usec_since_initialized(struct udev_device *udev_device) { (void)udev_device; return 0; }

const char *udev_device_get_sysattr_value(struct udev_device *udev_device, const char *sysattr) {
    struct stub_udev_device *d = (struct stub_udev_device *)udev_device;
    if (!d || !sysattr)
        return NULL;
    char path[320];
    snprintf(path, sizeof(path), "%s/%s", d->syspath, sysattr);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;
    static char buf[256];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return NULL;
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        n--;
    buf[n] = '\0';
    return buf;
}

struct udev_list_entry *udev_device_get_devlinks_list_entry(struct udev_device *udev_device) { (void)udev_device; return NULL; }
struct udev_list_entry *udev_device_get_properties_list_entry(struct udev_device *udev_device) { (void)udev_device; return NULL; }
struct udev_list_entry *udev_device_get_tags_list_entry(struct udev_device *udev_device) { (void)udev_device; return NULL; }
struct udev_list_entry *udev_device_get_sysattr_list_entry(struct udev_device *udev_device) { (void)udev_device; return NULL; }

const char *udev_device_get_property_value(struct udev_device *udev_device, const char *key) {
    struct stub_udev_device *d = (struct stub_udev_device *)udev_device;
    if (!d || !key)
        return NULL;
    /* Synthesized properties: the A20OS input multiplexer is a single
     * keyboard+mouse device; the display class needs no input properties. */
    if (is_input_subsystem(d->subsystem)) {
        if (strcmp(key, "ID_INPUT") == 0 ||
            strcmp(key, "ID_INPUT_KEYBOARD") == 0 ||
            strcmp(key, "ID_INPUT_MOUSE") == 0)
            return "1";
        if (strcmp(key, "ID_SEAT") == 0)
            return "seat0";
    }
    return NULL;
}
int udev_device_get_is_initialized(struct udev_device *udev_device) { (void)udev_device; return 1; }
int udev_device_has_tag(struct udev_device *udev_device, const char *tag) {
    struct stub_udev_device *d = (struct stub_udev_device *)udev_device;
    if (!d || !tag)
        return 0;
    if (strcmp(tag, "seat") == 0)
        return 1;
    return 0;
}
int udev_device_set_sysattr_value(struct udev_device *udev_device, const char *sysattr, const char *value) { (void)udev_device; (void)sysattr; (void)value; return -1; }

/* ------------------------------------------------------------------ */
/* udev_list_entry                                                     */
/* ------------------------------------------------------------------ */

struct udev_list_entry *udev_list_entry_get_next(struct udev_list_entry *list_entry) {
    struct stub_udev_list_entry *e = (struct stub_udev_list_entry *)list_entry;
    return e ? (struct udev_list_entry *)e->next : NULL;
}
struct udev_list_entry *udev_list_entry_get_by_name(struct udev_list_entry *list_entry, const char *name) {
    if (!name)
        return NULL;
    for (struct stub_udev_list_entry *e = (struct stub_udev_list_entry *)list_entry;
         e != NULL; e = e->next) {
        if (strcmp(e->name, name) == 0)
            return (struct udev_list_entry *)e;
    }
    return NULL;
}
const char *udev_list_entry_get_name(struct udev_list_entry *list_entry) {
    struct stub_udev_list_entry *e = (struct stub_udev_list_entry *)list_entry;
    return e ? e->name : NULL;
}
const char *udev_list_entry_get_value(struct udev_list_entry *list_entry) {
    struct stub_udev_list_entry *e = (struct stub_udev_list_entry *)list_entry;
    return e ? e->value : NULL;
}

/* ------------------------------------------------------------------ */
/* udev_enumerate                                                      */
/* ------------------------------------------------------------------ */

static struct stub_udev_list_entry *enumerate_prepend(
    struct stub_udev_enumerate *enumerate, const char *path)
{
    struct stub_udev_list_entry *e = calloc(1, sizeof(*e));
    if (!e)
        return enumerate->entries;
    snprintf(e->name, sizeof(e->name), "%s", path);
    e->next = enumerate->entries;
    enumerate->entries = e;
    return e;
}

/* Scan /sys/class/<subsystem>/ and append every entry that matches the
 * enumerate's subsystem filters.  Called by udev_enumerate_scan_devices. */
static void enumerate_scan_class(struct stub_udev_enumerate *enumerate,
                                 const char *subsystem)
{
    char dir[256];
    snprintf(dir, sizeof(dir), "/sys/class/%s", subsystem);
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.')
            continue;
        char path[320];
        snprintf(path, sizeof(path), "/sys/class/%s/%s", subsystem, de->d_name);
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode))
            continue;
        enumerate_prepend(enumerate, path);
    }
    closedir(d);
}

struct udev_enumerate *udev_enumerate_new(struct udev *udev) {
    (void)udev;
    struct stub_udev_enumerate *enumerate = calloc(1, sizeof(*enumerate));
    if (enumerate)
        enumerate->refs = 1;
    return (struct udev_enumerate *)enumerate;
}
struct udev_enumerate *udev_enumerate_ref(struct udev_enumerate *udev_enumerate) {
    struct stub_udev_enumerate *enumerate = (struct stub_udev_enumerate *)udev_enumerate;
    if (enumerate)
        enumerate->refs++;
    return udev_enumerate;
}
struct udev_enumerate *udev_enumerate_unref(struct udev_enumerate *udev_enumerate) {
    struct stub_udev_enumerate *enumerate = (struct stub_udev_enumerate *)udev_enumerate;
    if (!enumerate)
        return NULL;
    if (--enumerate->refs == 0) {
        struct stub_udev_list_entry *e = enumerate->entries;
        while (e) {
            struct stub_udev_list_entry *next = e->next;
            free(e);
            e = next;
        }
        free(enumerate);
    }
    return NULL;
}
struct udev *udev_enumerate_get_udev(struct udev_enumerate *udev_enumerate) { (void)udev_enumerate; return (struct udev *)&g_udev_dummy; }
int udev_enumerate_add_match_subsystem(struct udev_enumerate *udev_enumerate, const char *subsystem) {
    struct stub_udev_enumerate *enumerate = (struct stub_udev_enumerate *)udev_enumerate;
    if (enumerate && subsystem) {
        if (strcmp(subsystem, "input") == 0)
            enumerate->input = 1;
        else if (strcmp(subsystem, "display") == 0 || strcmp(subsystem, "graphics") == 0)
            enumerate->graphics = 1;
        else if (strcmp(subsystem, "drm") == 0)
            enumerate->drm = 1;
    }
    return 0;
}
int udev_enumerate_add_nomatch_subsystem(struct udev_enumerate *udev_enumerate, const char *subsystem) { (void)udev_enumerate; (void)subsystem; return 0; }
int udev_enumerate_add_match_sysattr(struct udev_enumerate *udev_enumerate, const char *sysattr, const char *value) { (void)udev_enumerate; (void)sysattr; (void)value; return 0; }
int udev_enumerate_add_nomatch_sysattr(struct udev_enumerate *udev_enumerate, const char *sysattr, const char *value) { (void)udev_enumerate; (void)sysattr; (void)value; return 0; }
int udev_enumerate_add_match_property(struct udev_enumerate *udev_enumerate, const char *property, const char *value) { (void)udev_enumerate; (void)property; (void)value; return 0; }
int udev_enumerate_add_match_sysname(struct udev_enumerate *udev_enumerate, const char *sysname) { (void)udev_enumerate; (void)sysname; return 0; }
int udev_enumerate_add_match_tag(struct udev_enumerate *udev_enumerate, const char *tag) { (void)udev_enumerate; (void)tag; return 0; }
int udev_enumerate_add_match_parent(struct udev_enumerate *udev_enumerate, struct udev_device *parent) { (void)udev_enumerate; (void)parent; return 0; }
int udev_enumerate_add_match_is_initialized(struct udev_enumerate *udev_enumerate) { (void)udev_enumerate; return 0; }
int udev_enumerate_add_syspath(struct udev_enumerate *udev_enumerate, const char *syspath) {
    struct stub_udev_enumerate *enumerate = (struct stub_udev_enumerate *)udev_enumerate;
    if (enumerate && syspath)
        enumerate_prepend(enumerate, syspath);
    return 0;
}
int udev_enumerate_scan_devices(struct udev_enumerate *udev_enumerate) {
    struct stub_udev_enumerate *enumerate = (struct stub_udev_enumerate *)udev_enumerate;
    if (!enumerate)
        return -1;
    if (enumerate->input)
        enumerate_scan_class(enumerate, "input");
    if (enumerate->graphics) {
        enumerate_scan_class(enumerate, "display");
        enumerate_scan_class(enumerate, "graphics");
    }
    if (enumerate->drm)
        enumerate_scan_class(enumerate, "drm");
    /* If no filter was set, report everything the caller might look for. */
    if (!enumerate->input && !enumerate->graphics && !enumerate->drm) {
        enumerate_scan_class(enumerate, "input");
        enumerate_scan_class(enumerate, "display");
        enumerate_scan_class(enumerate, "drm");
    }
    /* Fallback: if a requested class is not yet published in /sys (for
     * example, before a driver probes), synthesize the well-known A20OS
     * device nodes so device discovery still succeeds. */
    if (enumerate->input && enumerate->entries == NULL)
        enumerate_prepend(enumerate, "/sys/class/input/event0");
    if (enumerate->graphics && enumerate->entries == NULL)
        enumerate_prepend(enumerate, "/sys/class/display/fb0");
    if (enumerate->drm && enumerate->entries == NULL)
        enumerate_prepend(enumerate, "/sys/class/drm/card0");
    return 0;
}
int udev_enumerate_scan_subsystems(struct udev_enumerate *udev_enumerate) { (void)udev_enumerate; return 0; }
struct udev_list_entry *udev_enumerate_get_list_entry(struct udev_enumerate *udev_enumerate) {
    struct stub_udev_enumerate *enumerate = (struct stub_udev_enumerate *)udev_enumerate;
    return enumerate ? (struct udev_list_entry *)enumerate->entries : NULL;
}

/* ------------------------------------------------------------------ */
/* udev_monitor / queue / hwdb (no hotplug)                            */
/* ------------------------------------------------------------------ */

struct udev_monitor *udev_monitor_new_from_netlink(struct udev *udev, const char *name) {
    (void)udev;
    (void)name;
    struct stub_udev_monitor *monitor = calloc(1, sizeof(*monitor));
    if (!monitor)
        return NULL;
    monitor->fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (monitor->fd < 0) {
        free(monitor);
        return NULL;
    }
    monitor->refs = 1;
    return (struct udev_monitor *)monitor;
}
struct udev_monitor *udev_monitor_ref(struct udev_monitor *udev_monitor) {
    struct stub_udev_monitor *monitor = (struct stub_udev_monitor *)udev_monitor;
    if (monitor)
        monitor->refs++;
    return udev_monitor;
}
struct udev_monitor *udev_monitor_unref(struct udev_monitor *udev_monitor) {
    struct stub_udev_monitor *monitor = (struct stub_udev_monitor *)udev_monitor;
    if (monitor && --monitor->refs == 0) {
        close(monitor->fd);
        free(monitor);
    }
    return NULL;
}
struct udev *udev_monitor_get_udev(struct udev_monitor *udev_monitor) { (void)udev_monitor; return (struct udev *)&g_udev_dummy; }
int udev_monitor_enable_receiving(struct udev_monitor *udev_monitor) { return udev_monitor ? 0 : -1; }
int udev_monitor_set_receive_buffer_size(struct udev_monitor *udev_monitor, int size) { (void)udev_monitor; (void)size; return -1; }
int udev_monitor_get_fd(struct udev_monitor *udev_monitor) {
    struct stub_udev_monitor *monitor = (struct stub_udev_monitor *)udev_monitor;
    return monitor ? monitor->fd : -1;
}
struct udev_device *udev_monitor_receive_device(struct udev_monitor *udev_monitor) { (void)udev_monitor; return NULL; }
int udev_monitor_filter_add_match_subsystem_devtype(struct udev_monitor *udev_monitor, const char *subsystem, const char *devtype) { (void)udev_monitor; (void)subsystem; (void)devtype; return 0; }
int udev_monitor_filter_add_match_tag(struct udev_monitor *udev_monitor, const char *tag) { (void)udev_monitor; (void)tag; return 0; }
int udev_monitor_filter_update(struct udev_monitor *udev_monitor) { (void)udev_monitor; return 0; }
int udev_monitor_filter_remove(struct udev_monitor *udev_monitor) { (void)udev_monitor; return 0; }

struct udev_queue *udev_queue_new(struct udev *udev) { (void)udev; return NULL; }
struct udev_queue *udev_queue_ref(struct udev_queue *udev_queue) { return udev_queue; }
struct udev_queue *udev_queue_unref(struct udev_queue *udev_queue) { (void)udev_queue; return NULL; }
struct udev *udev_queue_get_udev(struct udev_queue *udev_queue) { (void)udev_queue; return (struct udev *)&g_udev_dummy; }
int udev_queue_get_udev_is_active(struct udev_queue *udev_queue) { (void)udev_queue; return 0; }
int udev_queue_get_queue_is_empty(struct udev_queue *udev_queue) { (void)udev_queue; return 1; }
int udev_queue_get_seqnum_is_finished(struct udev_queue *udev_queue, unsigned long long int seqnum) { (void)udev_queue; (void)seqnum; return 1; }
int udev_queue_get_seqnum_sequence_is_finished(struct udev_queue *udev_queue, unsigned long long int start, unsigned long long int end) { (void)udev_queue; (void)start; (void)end; return 1; }
int udev_queue_get_fd(struct udev_queue *udev_queue) { (void)udev_queue; return -1; }
int udev_queue_flush(struct udev_queue *udev_queue) { (void)udev_queue; return 0; }

struct udev_hwdb *udev_hwdb_new(struct udev *udev) { (void)udev; return NULL; }
struct udev_hwdb *udev_hwdb_ref(struct udev_hwdb *hwdb) { return hwdb; }
struct udev_hwdb *udev_hwdb_unref(struct udev_hwdb *hwdb) { (void)hwdb; return NULL; }
struct udev_list_entry *udev_hwdb_get_properties_list_entry(struct udev_hwdb *hwdb, const char *modalias, unsigned flags) { (void)hwdb; (void)modalias; (void)flags; return NULL; }
