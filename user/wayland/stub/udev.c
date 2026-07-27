/* Minimal libudev stub for A20OS: no device database, no hotplug.
 * Everything resolves to "no devices" / NULL so libinput's udev
 * discovery finds nothing; A20OS compositors use the libinput path
 * backend for the real /dev/event0 device. */

#include "udev.h"
#include <stdlib.h>
#include <string.h>

static int g_udev_dummy;

struct stub_udev_device {
    int refs;
    char devnode[64];
};

static struct stub_udev_device *stub_device_new(const char *devnode)
{
    struct stub_udev_device *d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->refs = 1;
    if (devnode) {
        strncpy(d->devnode, devnode, sizeof(d->devnode) - 1);
    } else {
        strcpy(d->devnode, "/dev/event0");
    }
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

struct udev_device *udev_device_new_from_syspath(struct udev *udev, const char *syspath) { (void)udev; (void)syspath; return (struct udev_device *)stub_device_new(NULL); }
struct udev_device *udev_device_new_from_devnum(struct udev *udev, char type, dev_t devnum) { (void)udev; (void)type; (void)devnum; return (struct udev_device *)stub_device_new(NULL); }
struct udev_device *udev_device_new_from_subsystem_sysname(struct udev *udev, const char *subsystem, const char *sysname) { (void)udev; (void)subsystem; (void)sysname; return NULL; }
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
const char *udev_device_get_devpath(struct udev_device *udev_device) { (void)udev_device; return "/virtual/event0"; }
const char *udev_device_get_subsystem(struct udev_device *udev_device) { (void)udev_device; return "input"; }
const char *udev_device_get_devtype(struct udev_device *udev_device) { (void)udev_device; return NULL; }
const char *udev_device_get_syspath(struct udev_device *udev_device) { (void)udev_device; return "/sys/devices/virtual/event0"; }
const char *udev_device_get_sysname(struct udev_device *udev_device) { (void)udev_device; return "event0"; }
const char *udev_device_get_sysnum(struct udev_device *udev_device) { (void)udev_device; return "0"; }
const char *udev_device_get_devnode(struct udev_device *udev_device) {
    struct stub_udev_device *d = (struct stub_udev_device *)udev_device;
    return d ? d->devnode : NULL;
}
dev_t udev_device_get_devnum(struct udev_device *udev_device) { (void)udev_device; return 0; }
const char *udev_device_get_driver(struct udev_device *udev_device) { (void)udev_device; return NULL; }
const char *udev_device_get_action(struct udev_device *udev_device) { (void)udev_device; return NULL; }
unsigned long long int udev_device_get_seqnum(struct udev_device *udev_device) { (void)udev_device; return 0; }
unsigned long long int udev_device_get_usec_since_initialized(struct udev_device *udev_device) { (void)udev_device; return 0; }
const char *udev_device_get_sysattr_value(struct udev_device *udev_device, const char *sysattr) { (void)udev_device; (void)sysattr; return NULL; }
struct udev_list_entry *udev_device_get_devlinks_list_entry(struct udev_device *udev_device) { (void)udev_device; return NULL; }
struct udev_list_entry *udev_device_get_properties_list_entry(struct udev_device *udev_device) { (void)udev_device; return NULL; }
struct udev_list_entry *udev_device_get_tags_list_entry(struct udev_device *udev_device) { (void)udev_device; return NULL; }
struct udev_list_entry *udev_device_get_sysattr_list_entry(struct udev_device *udev_device) { (void)udev_device; return NULL; }
const char *udev_device_get_property_value(struct udev_device *udev_device, const char *key) {
    (void)udev_device;
    if (!key)
        return NULL;
    if (strcmp(key, "ID_INPUT") == 0 ||
        strcmp(key, "ID_INPUT_KEYBOARD") == 0 ||
        strcmp(key, "ID_INPUT_MOUSE") == 0)
        return "1";
    if (strcmp(key, "ID_SEAT") == 0)
        return "seat0";
    return NULL;
}
int udev_device_get_is_initialized(struct udev_device *udev_device) { (void)udev_device; return 1; }
int udev_device_has_tag(struct udev_device *udev_device, const char *tag) { (void)udev_device; (void)tag; return 0; }
int udev_device_set_sysattr_value(struct udev_device *udev_device, const char *sysattr, const char *value) { (void)udev_device; (void)sysattr; (void)value; return -1; }

struct udev_list_entry *udev_list_entry_get_next(struct udev_list_entry *list_entry) { (void)list_entry; return NULL; }
struct udev_list_entry *udev_list_entry_get_by_name(struct udev_list_entry *list_entry, const char *name) { (void)list_entry; (void)name; return NULL; }
const char *udev_list_entry_get_name(struct udev_list_entry *list_entry) { (void)list_entry; return NULL; }
const char *udev_list_entry_get_value(struct udev_list_entry *list_entry) { (void)list_entry; return NULL; }

struct udev_enumerate *udev_enumerate_new(struct udev *udev) { (void)udev; return NULL; }
struct udev_enumerate *udev_enumerate_ref(struct udev_enumerate *udev_enumerate) { return udev_enumerate; }
struct udev_enumerate *udev_enumerate_unref(struct udev_enumerate *udev_enumerate) { (void)udev_enumerate; return NULL; }
struct udev *udev_enumerate_get_udev(struct udev_enumerate *udev_enumerate) { (void)udev_enumerate; return NULL; }
int udev_enumerate_add_match_subsystem(struct udev_enumerate *udev_enumerate, const char *subsystem) { (void)udev_enumerate; (void)subsystem; return 0; }
int udev_enumerate_add_nomatch_subsystem(struct udev_enumerate *udev_enumerate, const char *subsystem) { (void)udev_enumerate; (void)subsystem; return 0; }
int udev_enumerate_add_match_sysattr(struct udev_enumerate *udev_enumerate, const char *sysattr, const char *value) { (void)udev_enumerate; (void)sysattr; (void)value; return 0; }
int udev_enumerate_add_nomatch_sysattr(struct udev_enumerate *udev_enumerate, const char *sysattr, const char *value) { (void)udev_enumerate; (void)sysattr; (void)value; return 0; }
int udev_enumerate_add_match_property(struct udev_enumerate *udev_enumerate, const char *property, const char *value) { (void)udev_enumerate; (void)property; (void)value; return 0; }
int udev_enumerate_add_match_sysname(struct udev_enumerate *udev_enumerate, const char *sysname) { (void)udev_enumerate; (void)sysname; return 0; }
int udev_enumerate_add_match_tag(struct udev_enumerate *udev_enumerate, const char *tag) { (void)udev_enumerate; (void)tag; return 0; }
int udev_enumerate_add_match_parent(struct udev_enumerate *udev_enumerate, struct udev_device *parent) { (void)udev_enumerate; (void)parent; return 0; }
int udev_enumerate_add_match_is_initialized(struct udev_enumerate *udev_enumerate) { (void)udev_enumerate; return 0; }
int udev_enumerate_add_syspath(struct udev_enumerate *udev_enumerate, const char *syspath) { (void)udev_enumerate; (void)syspath; return 0; }
int udev_enumerate_scan_devices(struct udev_enumerate *udev_enumerate) { (void)udev_enumerate; return 0; }
int udev_enumerate_scan_subsystems(struct udev_enumerate *udev_enumerate) { (void)udev_enumerate; return 0; }
struct udev_list_entry *udev_enumerate_get_list_entry(struct udev_enumerate *udev_enumerate) { (void)udev_enumerate; return NULL; }

struct udev_monitor *udev_monitor_new_from_netlink(struct udev *udev, const char *name) { (void)udev; (void)name; return NULL; }
struct udev_monitor *udev_monitor_ref(struct udev_monitor *udev_monitor) { return udev_monitor; }
struct udev_monitor *udev_monitor_unref(struct udev_monitor *udev_monitor) { (void)udev_monitor; return NULL; }
struct udev *udev_monitor_get_udev(struct udev_monitor *udev_monitor) { (void)udev_monitor; return NULL; }
int udev_monitor_enable_receiving(struct udev_monitor *udev_monitor) { (void)udev_monitor; return -1; }
int udev_monitor_set_receive_buffer_size(struct udev_monitor *udev_monitor, int size) { (void)udev_monitor; (void)size; return -1; }
int udev_monitor_get_fd(struct udev_monitor *udev_monitor) { (void)udev_monitor; return -1; }
struct udev_device *udev_monitor_receive_device(struct udev_monitor *udev_monitor) { (void)udev_monitor; return NULL; }
int udev_monitor_filter_add_match_subsystem_devtype(struct udev_monitor *udev_monitor, const char *subsystem, const char *devtype) { (void)udev_monitor; (void)subsystem; (void)devtype; return 0; }
int udev_monitor_filter_add_match_tag(struct udev_monitor *udev_monitor, const char *tag) { (void)udev_monitor; (void)tag; return 0; }
int udev_monitor_filter_update(struct udev_monitor *udev_monitor) { (void)udev_monitor; return 0; }
int udev_monitor_filter_remove(struct udev_monitor *udev_monitor) { (void)udev_monitor; return 0; }

struct udev_queue *udev_queue_new(struct udev *udev) { (void)udev; return NULL; }
struct udev_queue *udev_queue_ref(struct udev_queue *udev_queue) { return udev_queue; }
struct udev_queue *udev_queue_unref(struct udev_queue *udev_queue) { (void)udev_queue; return NULL; }
struct udev *udev_queue_get_udev(struct udev_queue *udev_queue) { (void)udev_queue; return NULL; }
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
