/* SPDX-License-Identifier: Apache-2.0 */
#ifndef CONFIG_MCU
#include "drivers/core/runtime_test.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/fence.h"
#include "drivers/core/workqueue.h"
#include "drivers/core/firmware.h"
#include "core/bootargs.h"
#include "core/errno.h"
#include "core/klog.h"
#include "core/panic.h"
#include "core/string.h"
#include "core/timer.h"
#include "core/fcntl.h"
#include "fs/vfs.h"
#include "fs/file.h"

#define CHECK(test) do { if (!(test)) \
    panic("DRIVER_RUNTIME: FAIL line=%d: %s", __LINE__, #test); } while (0)

struct runtime_case {
    struct driver_workqueue *queue;
    struct driver_work *work;
    struct driver_fence *entered, *release, *done;
    unsigned count;
    int error;
};

static uint64_t deadline(void) { return clock_get_ticks() + MS_TO_TICKS(2000); }

static void delayed_callback(void *opaque)
{
    struct runtime_case *test = opaque;
    test->count++;
    CHECK(!driver_fence_signal(test->done, 0));
}

static void requeue_callback(void *opaque)
{
    struct runtime_case *test = opaque;
    if (++test->count == 1) {
        CHECK(driver_work_schedule(test->work, 0) == 1);
    } else {
        CHECK(driver_work_cancel_sync(test->work, 0) == -EDEADLK);
        CHECK(!driver_fence_signal(test->done, 0));
    }
}

static void blocked_callback(void *opaque)
{
    struct runtime_case *test = opaque;
    CHECK(!driver_fence_signal(test->entered, 0));
    test->error = driver_fence_wait(test->release, deadline(), false);
    test->count++;
}

static void exercise(void)
{
    struct runtime_case test = {0};
    test.queue = driver_workqueue_create();
    test.done = driver_fence_create();
    CHECK(test.queue && test.done);
    test.work = driver_work_create(test.queue, delayed_callback, &test);
    CHECK(test.work);
    uint64_t due = clock_get_ticks() + MS_TO_TICKS(15);
    CHECK(driver_work_schedule(test.work, due) == 1);
    CHECK(driver_work_schedule(test.work, 0) == 0);
    CHECK(!driver_fence_wait(test.done, deadline(), false));
    CHECK(clock_get_ticks() >= due);
    CHECK(!driver_workqueue_flush(test.queue, deadline()));
    CHECK(test.count == 1);
    driver_fence_put(test.done);
    test.done = driver_fence_create();
    CHECK(test.done);
    CHECK(driver_work_schedule(test.work, clock_get_ticks() + MS_TO_TICKS(10000)) == 1);
    due = clock_get_ticks() + MS_TO_TICKS(15);
    CHECK(driver_work_reschedule(test.work, due) == 0);
    CHECK(!driver_fence_wait(test.done, deadline(), false));
    CHECK(!driver_workqueue_flush(test.queue, deadline()));
    CHECK(test.count == 2 && clock_get_ticks() >= due);
    CHECK(driver_work_schedule(test.work, clock_get_ticks() + MS_TO_TICKS(10000)) == 1);
    CHECK(!driver_work_cancel_sync(test.work, deadline()));
    CHECK(!driver_workqueue_flush(test.queue, deadline()) && test.count == 2);
    driver_work_put(test.work);
    driver_fence_put(test.done);

    test.count = 0;
    test.done = driver_fence_create();
    test.work = driver_work_create(test.queue, requeue_callback, &test);
    CHECK(test.done && test.work);
    CHECK(driver_work_schedule(test.work, 0) == 1);
    CHECK(!driver_fence_wait(test.done, deadline(), false));
    CHECK(!driver_workqueue_flush(test.queue, deadline()) && test.count == 2);
    driver_work_put(test.work);
    driver_fence_put(test.done);

    test.count = 0;
    test.entered = driver_fence_create();
    test.release = driver_fence_create();
    test.work = driver_work_create(test.queue, blocked_callback, &test);
    CHECK(test.entered && test.release && test.work);
    CHECK(driver_work_schedule(test.work, 0) == 1);
    CHECK(!driver_fence_wait(test.entered, deadline(), false));
    CHECK(driver_workqueue_destroy(&test.queue,
          clock_get_ticks() + MS_TO_TICKS(5)) == -ETIMEDOUT);
    CHECK(test.queue && driver_work_schedule(test.work, 0) == -ESHUTDOWN);
    CHECK(!driver_fence_signal(test.release, 0));
    CHECK(!driver_workqueue_destroy(&test.queue, deadline()) && !test.queue);
    CHECK(test.count == 1 && !test.error);
    CHECK(driver_work_schedule(test.work, 0) == -ESHUTDOWN);
    driver_work_put(test.work);
    driver_fence_put(test.entered);
    driver_fence_put(test.release);
}

static void exercise_firmware(void)
{
    const char bytes[] = {'A', '2', '0', '\0', 1, 2, 3};
    const char *root = "/driver-firmware-test";
    const char *subdir = "/driver-firmware-test/vendor";
    const char *path = "/driver-firmware-test/vendor/gpu.bin";
    const char *alias = "/driver-firmware-test/alias";
    struct driver_firmware *image;
    CHECK(!driver_firmware_from_memory(bytes, sizeof(bytes), sizeof(bytes), &image));
    CHECK(driver_firmware_size(image) == sizeof(bytes));
    CHECK(!memcmp(driver_firmware_data(image), bytes, sizeof(bytes)));
    driver_firmware_put(image);
    CHECK(!vfs_mkdir(root, 0755) && !vfs_mkdir(subdir, 0755));
    int fd = vfs_open(path, O_CREAT | O_WRONLY, 0644);
    CHECK(fd >= 0);
    CHECK(vfs_write(fd, bytes, sizeof(bytes)) == sizeof(bytes));
    CHECK(!vfs_close(fd));
    fd = vfs_open(root, O_RDONLY | O_DIRECTORY, 0);
    CHECK(fd >= 0);
    vfile_t *directory = vfs_get_file_ref(fd);
    struct driver_firmware_source *source;
    CHECK(directory && !driver_firmware_source_create_vfs(directory, &source));
    vfs_put_file(directory);
    CHECK(!vfs_close(fd)); /* Loads must outlive the caller's descriptor. */
    CHECK(driver_firmware_load(source, "../outside", 128, &image) == -EINVAL);
    CHECK(driver_firmware_load(source, "missing", 128, &image) == -ENOENT);
    CHECK(!vfs_symlink("vendor/gpu.bin", alias));
    CHECK(driver_firmware_load(source, "alias", 128, &image) == -ELOOP);
    CHECK(driver_firmware_load(source, "vendor/gpu.bin", sizeof(bytes)-1, &image) == -EFBIG);
    CHECK(!driver_firmware_load(source, "vendor/gpu.bin", sizeof(bytes), &image));
    driver_firmware_source_put(source);
    CHECK(!vfs_unlink(alias) && !vfs_unlink(path));
    CHECK(!vfs_rmdir(subdir) && !vfs_rmdir(root));
    /* The image owns its bytes independently of the source and inode. */
    CHECK(driver_firmware_size(image) == sizeof(bytes));
    CHECK(!memcmp(driver_firmware_data(image), bytes, sizeof(bytes)));
    driver_firmware_put(image);
}

void driver_runtime_selftest_boot(void)
{
    const char *args = bootargs_get();
    const char *key = "driver.runtime_test=1";
    const char *at = args;
    bool requested = false;
    while (at && (at = strstr(at, key))) {
        char next = at[strlen(key)];
        if ((at == args || at[-1] == ' ' || at[-1] == '\t') &&
            (!next || next == ' ' || next == '\t')) {
            requested = true;
            break;
        }
        at++;
    }
    if (!requested)
        return;
    kinfo("DRIVER_RUNTIME: START\n");
    exercise();
    exercise_firmware();
    kinfo("DRIVER_RUNTIME: PASS delayed=1 retime=1 cancel=1 requeue=1 destroy_timeout=1 park_wake=1 firmware=1\n");
    if (current_board && current_board->poweroff)
        current_board->poweroff();
    panic("DRIVER_RUNTIME: poweroff returned");
}
#endif
