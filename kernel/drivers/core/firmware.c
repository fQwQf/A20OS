/* SPDX-License-Identifier: Apache-2.0 */
#ifndef CONFIG_MCU
#include "drivers/core/firmware.h"
#include "core/errno.h"
#include "core/refcount.h"
#include "core/string.h"
#include "mm/slab.h"

struct driver_firmware_source {
    refcount_t references;
    struct driver_firmware_ops ops;
    void *context;
};

struct driver_firmware {
    refcount_t references;
    size_t size;
    unsigned char data[];
};

int driver_firmware_source_create(const struct driver_firmware_ops *ops,
                                  void *context,
                                  struct driver_firmware_source **out)
{
    if (!out)
        return -EINVAL;
    *out = NULL;
    if (!ops || !ops->open || !ops->read || !ops->close)
        return -EINVAL;
    struct driver_firmware_source *source = kmalloc(sizeof(*source));
    if (!source)
        return -ENOMEM;
    refcount_set(&source->references, 1);
    source->ops = *ops;
    source->context = context;
    *out = source;
    return 0;
}

void driver_firmware_source_get(struct driver_firmware_source *source)
{
    if (source)
        refcount_inc(&source->references);
}

void driver_firmware_source_put(struct driver_firmware_source *source)
{
    if (!source || !refcount_dec_and_test(&source->references))
        return;
    if (source->ops.release)
        source->ops.release(source->context);
    kfree(source);
}

static int validate_name(const char *name)
{
    if (!name || !*name)
        return -EINVAL;
    size_t length = 0;
    while (length <= DRIVER_FIRMWARE_NAME_MAX && name[length])
        length++;
    if (length > DRIVER_FIRMWARE_NAME_MAX)
        return -ENAMETOOLONG;
    const char *part = name;
    for (size_t i = 0; i <= length; i++) {
        if (name[i] && name[i] != '/')
            continue;
        size_t count = name + i - part;
        if (!count || (count == 1 && part[0] == '.') ||
            (count == 2 && part[0] == '.' && part[1] == '.'))
            return -EINVAL;
        part = name + i + 1;
    }
    return 0;
}

static int allocate_image(uint64_t size, size_t max_size,
                           struct driver_firmware **out)
{
    if (!size)
        return -ENODATA;
    if (size > max_size || size > DRIVER_FIRMWARE_SIZE_MAX)
        return -EFBIG;
    struct driver_firmware *image = kmalloc(sizeof(*image) + (size_t)size);
    if (!image)
        return -ENOMEM;
    refcount_set(&image->references, 1);
    image->size = (size_t)size;
    *out = image;
    return 0;
}

int driver_firmware_load(struct driver_firmware_source *source, const char *name,
                          size_t max_size, struct driver_firmware **out)
{
    if (!out)
        return -EINVAL;
    *out = NULL;
    if (!source || !max_size)
        return -EINVAL;
    int ret = validate_name(name);
    if (ret)
        return ret;
    driver_firmware_source_get(source);
    void *stream = NULL;
    uint64_t size = 0;
    struct driver_firmware *image = NULL;
    ret = source->ops.open(source->context, name, &stream, &size);
    if (ret) {
        ret = ret < 0 ? ret : -EIO;
        goto put_source;
    }
    ret = allocate_image(size, max_size, &image);
    if (ret)
        goto close_stream;
    size_t offset = 0;
    while (offset < image->size) {
        size_t count = image->size - offset;
        if (count > 65536)
            count = 65536;
        ret = source->ops.read(source->context, stream, offset,
                                image->data + offset, count);
        if (ret <= 0 || (size_t)ret > count) {
            ret = ret < 0 ? ret : -EIO;
            goto close_stream;
        }
        offset += (size_t)ret;
    }
    /* A larger-than-advertised stream is not a complete firmware image. */
    unsigned char extra;
    ret = source->ops.read(source->context, stream, offset, &extra, 1);
    if (ret)
        ret = ret < 0 ? ret : -EIO;
close_stream:
    source->ops.close(source->context, stream);
    if (!ret)
        *out = image;
    else
        driver_firmware_put(image);
put_source:
    driver_firmware_source_put(source);
    return ret;
}

int driver_firmware_from_memory(const void *data, size_t size, size_t max_size,
                                 struct driver_firmware **out)
{
    if (!out)
        return -EINVAL;
    *out = NULL;
    if (!data || !max_size)
        return -EINVAL;
    int ret = allocate_image(size, max_size, out);
    if (!ret)
        memcpy((*out)->data, data, size);
    return ret;
}

void driver_firmware_get(struct driver_firmware *firmware)
{
    if (firmware)
        refcount_inc(&firmware->references);
}

void driver_firmware_put(struct driver_firmware *firmware)
{
    if (firmware && refcount_dec_and_test(&firmware->references))
        kfree(firmware);
}

const void *driver_firmware_data(const struct driver_firmware *firmware)
{
    return firmware ? firmware->data : NULL;
}

size_t driver_firmware_size(const struct driver_firmware *firmware)
{
    return firmware ? firmware->size : 0;
}
#endif
