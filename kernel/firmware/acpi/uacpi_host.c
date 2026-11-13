#include "firmware/acpi.h"
#include "core/errno.h"
#ifdef CONFIG_UACPI
#include "core/defs.h"
#include "mm/slab.h"
#include "core/string.h"
#include "core/stdio.h"
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>
#include <uacpi/resources.h>
#include <uacpi/sleep.h>

typedef struct acpi_node_record {
    struct acpi_node_record *all_next, *parent, *first_child, *last_child,
        *next;
    uacpi_namespace_node *handle;
    char name[5];
    uacpi_namespace_node_info *info;
    uacpi_resources *crs;
    fw_resource_t *resources;
    struct acpi_irq {
        uint32_t number, flags;
    } *irqs;
    size_t resource_count, irq_count;
    char *cid;
    size_t cid_size;
    uint32_t sta;
    uint32_t i2c_address, i2c_speed, hid_desc_register;
    uacpi_namespace_node *gpio_attention;
    uint16_t gpio_pin;
    uint8_t gpio_active_low;
    int hid_desc_error;
    int sta_error, crs_error;
} acpi_node_record_t;
static acpi_context_t active_context;
static acpi_node_record_t *records, *root_record;
static int namespace_state, snapshot_error;
static uint32_t tpm2_start_method;
static int tpm2_method_valid;
static const fw_node_ops_t acpi_ops;

uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr *out) {
    if (!out || !active_context.rsdp)
        return UACPI_STATUS_NOT_FOUND;
    *out = active_context.rsdp;
    return UACPI_STATUS_OK;
}
void *uacpi_kernel_map(uacpi_phys_addr pa, uacpi_size size) {
    if (!active_context.mapper.map || !size || size - 1 > ~(uint64_t)0 - pa)
        return UACPI_MAP_FAILED;
    const void *p =
        active_context.mapper.map(active_context.mapper.opaque, pa, size);
    if (!p && active_context.mapper.map_region)
        p = active_context.mapper.map_region(active_context.mapper.opaque, pa,
                                             size);
    return p ? (void *)p : UACPI_MAP_FAILED;
}
void uacpi_kernel_unmap(void *ptr, uacpi_size size) {
    if (ptr && ptr != UACPI_MAP_FAILED && active_context.mapper.unmap)
        active_context.mapper.unmap(active_context.mapper.opaque, ptr, size);
}
void uacpi_kernel_log(uacpi_log_level level, const uacpi_char *text) {
    (void)level;
    printf("[uACPI] %s", text);
}
static int error(uacpi_status status) {
    if (status == UACPI_STATUS_OK)
        return 0;
    if (status == UACPI_STATUS_NOT_FOUND)
        return -ENOENT;
    if (status == UACPI_STATUS_OUT_OF_MEMORY)
        return -ENOMEM;
    return -EINVAL;
}
static fw_node_t node_for(acpi_node_record_t *r) {
    return (fw_node_t){&acpi_ops, r, (uint64_t)(uintptr_t)r->handle};
}
static acpi_node_record_t *record_for(const fw_node_t *n) {
    return n && n->ops == &acpi_ops ? (acpi_node_record_t *)n->context : NULL;
}
static acpi_node_record_t *record_find(uacpi_namespace_node *handle) {
    for (acpi_node_record_t *r = records; r; r = r->all_next)
        if (r->handle == handle)
            return r;
    return NULL;
}
static acpi_node_record_t *record_add(uacpi_namespace_node *handle) {
    if (!handle)
        return NULL;
    if (uacpi_namespace_node_depth(handle) > 64) {
        snapshot_error = -E2BIG;
        return NULL;
    }
    acpi_node_record_t *r = record_find(handle);
    if (r)
        return r;
    acpi_node_record_t *parent =
        record_add(uacpi_namespace_node_parent(handle));
    if (snapshot_error)
        return NULL;
    r = kmalloc(sizeof(*r));
    if (!r) {
        snapshot_error = -ENOMEM;
        return NULL;
    }
    memset(r, 0, sizeof(*r));
    r->handle = handle;
    r->parent = parent;
    uacpi_object_name name = uacpi_namespace_node_name(handle);
    memcpy(r->name, name.text, 4);
    r->all_next = records;
    records = r;
    r->sta = 0xf;
    r->hid_desc_error = -ENOENT;
    if (parent) {
        if (parent->last_child)
            parent->last_child->next = r;
        else
            parent->first_child = r;
        parent->last_child = r;
    }
    return r;
}
static int append_resource(acpi_node_record_t *r, enum fw_resource_type type,
                           uint64_t address, uint64_t length) {
    if (!length || length - 1 > ~(uint64_t)0 - address)
        return -EINVAL;
    if (r->resource_count >= 256)
        return 0;
    size_t bytes = (r->resource_count + 1) * sizeof(*r->resources);
    fw_resource_t *new = kmalloc(bytes);
    if (!new)
        return -ENOMEM;
    if (r->resources)
        memcpy(new, r->resources, bytes - sizeof(*new));
    new[r->resource_count++] =
        (fw_resource_t){type, address, length, 0, "_CRS"};
    kfree(r->resources);
    r->resources = new;
    return 0;
}
static int append_irq(acpi_node_record_t *r, uint32_t number,
                      unsigned triggering, unsigned polarity) {
    if (polarity > 1 || triggering > 1)
        return -EINVAL;
    if (r->irq_count >= 256)
        return 0;
    size_t bytes = (r->irq_count + 1) * sizeof(*r->irqs);
    struct acpi_irq *new = kmalloc(bytes);
    if (!new)
        return -ENOMEM;
    if (r->irqs)
        memcpy(new, r->irqs, bytes - sizeof(*new));
    unsigned flags = triggering == UACPI_TRIGGERING_EDGE ? (polarity ? 2 : 1)
                                                         : (polarity ? 8 : 4);
    new[r->irq_count++] = (struct acpi_irq){number, flags};
    kfree(r->irqs);
    r->irqs = new;
    return 0;
}
static int address_resource(acpi_node_record_t *r,
                            const uacpi_resource_address_common *c,
                            uint64_t min, uint64_t max, uint64_t translation,
                            uint64_t len) {
    if (c->direction == UACPI_PRODUCER || c->type == UACPI_RANGE_BUS)
        return 0;
    if (c->type != UACPI_RANGE_MEMORY && c->type != UACPI_RANGE_IO)
        return 0;
    /*
     * A range descriptor is not one CPU resource.  Real laptop PCI root
     * bridges publish QWord/DWord memory ranges with min != max and the
     * fixed-address flags clear; the old -EINVAL here poisoned the bridge's
     * _CRS and aborted the whole firmware enumeration before VFS.  Keep the
     * raw _CRS for the device driver and skip the range in the generic
     * resource list.
     */
    if (!c->fixed_min_address || !c->fixed_max_address || min != max)
        return 0;
    if (!len)
        len = 1;
    if (translation > ~(uint64_t)0 - min ||
        len - 1 > ~(uint64_t)0 - (min + translation))
        return 0;
    return append_resource(
        r, c->type == UACPI_RANGE_IO ? FW_RESOURCE_IO : FW_RESOURCE_MEMORY,
        min + translation, len);
}
static uacpi_iteration_decision resource_snapshot(void *opaque,
                                                  uacpi_resource *res) {
    acpi_node_record_t *r = opaque;
    int rc = 0;
    switch (res->type) {
        case UACPI_RESOURCE_TYPE_IO:
            if (res->io.minimum != res->io.maximum)
                break;
            rc = append_resource(r, FW_RESOURCE_IO, res->io.minimum,
                                 res->io.length);
            break;
        case UACPI_RESOURCE_TYPE_FIXED_IO:
            rc = append_resource(r, FW_RESOURCE_IO, res->fixed_io.address,
                                 res->fixed_io.length);
            break;
        case UACPI_RESOURCE_TYPE_MEMORY24:
            if (res->memory24.minimum != res->memory24.maximum)
                break;
            rc = append_resource(r, FW_RESOURCE_MEMORY,
                                 (uint64_t)res->memory24.minimum << 8,
                                 (uint64_t)res->memory24.length << 8);
            break;
        case UACPI_RESOURCE_TYPE_MEMORY32:
            if (res->memory32.minimum != res->memory32.maximum)
                break;
            rc = append_resource(r, FW_RESOURCE_MEMORY, res->memory32.minimum,
                                 res->memory32.length);
            break;
        case UACPI_RESOURCE_TYPE_FIXED_MEMORY32:
            rc = append_resource(r, FW_RESOURCE_MEMORY,
                                 res->fixed_memory32.address,
                                 res->fixed_memory32.length);
            break;
#define ADDRESS_CASE(bits, member)                                             \
    case UACPI_RESOURCE_TYPE_ADDRESS##bits:                                    \
        rc = address_resource(                                                 \
            r, &res->member.common, res->member.minimum, res->member.maximum,  \
            res->member.translation_offset, res->member.address_length);       \
        break
            ADDRESS_CASE(16, address16);
            ADDRESS_CASE(32, address32);
            ADDRESS_CASE(64, address64);
            ADDRESS_CASE(64_EXTENDED, address64_extended);
#undef ADDRESS_CASE
        case UACPI_RESOURCE_TYPE_IRQ:
            for (unsigned i = 0; !rc && i < res->irq.num_irqs; i++)
                rc = append_irq(r, res->irq.irqs[i], res->irq.triggering,
                                res->irq.polarity);
            break;
        case UACPI_RESOURCE_TYPE_EXTENDED_IRQ:
            if (res->extended_irq.direction == UACPI_PRODUCER)
                break;
            if (res->extended_irq.source.length)
                break;
            for (unsigned i = 0; !rc && i < res->extended_irq.num_irqs; i++)
                rc = append_irq(r, res->extended_irq.irqs[i],
                                res->extended_irq.triggering,
                                res->extended_irq.polarity);
            break;
        case UACPI_RESOURCE_TYPE_SERIAL_I2C_CONNECTION:
            if (!r->i2c_address &&
                res->i2c_connection.addressing_mode == UACPI_I2C_7BIT &&
                res->i2c_connection.slave_address <= 0x7f) {
                r->i2c_address = res->i2c_connection.slave_address;
                r->i2c_speed = res->i2c_connection.connection_speed;
            }
            break;
        case UACPI_RESOURCE_TYPE_GPIO_CONNECTION: {
            const uacpi_resource_gpio_connection *gpio = &res->gpio_connection;
            if (r->gpio_attention ||
                gpio->type != UACPI_GPIO_CONNECTION_INTERRUPT ||
                gpio->direction != UACPI_CONSUMER ||
                gpio->intr.triggering != UACPI_TRIGGERING_LEVEL ||
                gpio->intr.polarity > UACPI_POLARITY_ACTIVE_LOW ||
                gpio->pin_table_length != 1 || !gpio->source.length)
                break;
            uacpi_namespace_node *provider;
            if (!uacpi_namespace_node_resolve_from_aml_namepath(
                    r->handle, gpio->source.string, &provider)) {
                r->gpio_attention = provider;
                r->gpio_pin = gpio->pin_table[0];
                r->gpio_active_low = gpio->intr.polarity == UACPI_POLARITY_ACTIVE_LOW;
            }
            break;
        }
        /* The remaining connection descriptors are not CPU resources. */
        case UACPI_RESOURCE_TYPE_SERIAL_SPI_CONNECTION:
        case UACPI_RESOURCE_TYPE_SERIAL_UART_CONNECTION:
        case UACPI_RESOURCE_TYPE_SERIAL_CSI2_CONNECTION:
        case UACPI_RESOURCE_TYPE_PIN_FUNCTION:
        case UACPI_RESOURCE_TYPE_PIN_CONFIGURATION:
        case UACPI_RESOURCE_TYPE_PIN_GROUP:
        case UACPI_RESOURCE_TYPE_PIN_GROUP_FUNCTION:
        case UACPI_RESOURCE_TYPE_PIN_GROUP_CONFIGURATION:
        case UACPI_RESOURCE_TYPE_GENERIC_REGISTER:
            break;
        case UACPI_RESOURCE_TYPE_END_TAG:
        case UACPI_RESOURCE_TYPE_VENDOR_SMALL:
        case UACPI_RESOURCE_TYPE_VENDOR_LARGE:
            break;
        default:
            /* Unknown descriptor: keep it in the decoded _CRS, do not poison the
             * device's whole resource list. */
            break;
    }
    if (rc)
        r->crs_error = rc;
    return rc ? UACPI_ITERATION_DECISION_BREAK
              : UACPI_ITERATION_DECISION_CONTINUE;
}

static int record_matches(acpi_node_record_t *r, const char *id);

static int hid_descriptor_register(acpi_node_record_t *r)
{
    /* HID-over-I2C _DSM, revision 1, function 1. UUID in ACPI byte order. */
    static const uint8_t guid[] = {
        0xf7, 0xf6, 0xdf, 0x3c, 0x67, 0x42, 0x55, 0x45,
        0xad, 0x05, 0xb3, 0x0a, 0x3d, 0x89, 0x38, 0xde,
    };
    uacpi_object *args[] = {
        uacpi_object_create_buffer((uacpi_data_view){
            .const_bytes = guid, .length = sizeof(guid),
        }),
        uacpi_object_create_integer(1),
        uacpi_object_create_integer(1),
        uacpi_object_create_package((uacpi_object_array){0}),
    };
    uint64_t value;
    int rc = -ENOMEM;
    if (args[0] && args[1] && args[2] && args[3]) {
        uacpi_object_array params = {.objects = args, .count = ARRAY_SIZE(args)};
        rc = error(uacpi_eval_integer(r->handle, "_DSM", &params, &value));
        if (!rc) {
            if (value > 0xffffU)
                rc = -EINVAL;
            else
                r->hid_desc_register = (uint32_t)value;
        }
    }
    for (size_t i = 0; i < ARRAY_SIZE(args); i++)
        uacpi_object_unref(args[i]);
    return rc;
}

static uacpi_iteration_decision
snapshot(void *opaque, uacpi_namespace_node *node, uacpi_u32 depth) {
    (void)opaque;
    (void)depth;
    acpi_node_record_t *r = record_add(node);
    if (!r)
        return UACPI_ITERATION_DECISION_BREAK;
    uacpi_status status = uacpi_get_namespace_node_info(node, &r->info);
    if (status) {
        snapshot_error = error(status);
        return UACPI_ITERATION_DECISION_BREAK;
    }
    r->sta_error = error(uacpi_eval_sta(node, &r->sta));
    if (r->sta_error || !(r->sta & (1U | 8U)))
        return UACPI_ITERATION_DECISION_NEXT_PEER;
    if (!(r->sta & 1U))
        return UACPI_ITERATION_DECISION_CONTINUE;
    size_t cid_size = 0;
    for (unsigned i = 0; i < r->info->cid.num_ids; i++) {
        if (r->info->cid.ids[i].size > ~(size_t)0 - cid_size) {
            snapshot_error = -E2BIG;
            return UACPI_ITERATION_DECISION_BREAK;
        }
        cid_size += r->info->cid.ids[i].size;
    }
    if (cid_size) {
        r->cid = kmalloc(cid_size);
        if (!r->cid) {
            snapshot_error = -ENOMEM;
            return UACPI_ITERATION_DECISION_BREAK;
        }
        r->cid_size = cid_size;
        size_t off = 0;
        for (unsigned i = 0; i < r->info->cid.num_ids; i++) {
            uacpi_id_string *id = &r->info->cid.ids[i];
            memcpy(r->cid + off, id->value, id->size);
            off += id->size;
        }
    }
    status = uacpi_get_current_resources(node, &r->crs);
    if (status != UACPI_STATUS_NOT_FOUND) {
        r->crs_error = error(status);
        if (!status) {
            status = uacpi_for_each_resource(r->crs, resource_snapshot, r);
            if (status)
                r->crs_error = error(status);
        }
    }
    if (r->i2c_address && record_matches(r, "PNP0C50"))
        r->hid_desc_error = hid_descriptor_register(r);
    return UACPI_ITERATION_DECISION_CONTINUE;
}
static const char *node_name(const fw_node_t *n) {
    return record_for(n)->name;
}
static int parent(const fw_node_t *n, fw_node_t *out) {
    acpi_node_record_t *r = record_for(n);
    if (!r || !out)
        return -EINVAL;
    if (!r->parent)
        return -ENOENT;
    *out = node_for(r->parent);
    return 0;
}
static int next_child(const fw_node_t *n, const fw_node_t *prev,
                      fw_node_t *out) {
    acpi_node_record_t *r = record_for(n), *p = record_for(prev);
    if (!r || !out || (prev && (!p || p->parent != r)))
        return -EINVAL;
    r = prev ? p->next : r->first_child;
    if (!r)
        return -ENOENT;
    *out = node_for(r);
    return 0;
}
static int record_matches(acpi_node_record_t *r, const char *id) {
    if (!r || !r->info || !id)
        return 0;
    if (r->info->hid.value && !strcmp(r->info->hid.value, id))
        return 1;
    for (unsigned i = 0; i < r->info->cid.num_ids; i++)
        if (!strcmp(r->info->cid.ids[i].value, id))
            return 1;
    return 0;
}
static int property(const fw_node_t *n, const char *name, fw_property_t *out) {
    acpi_node_record_t *r = record_for(n);
    if (!r || !out || !name)
        return -EINVAL;
    const void *data = NULL;
    size_t size = 0;
    if (!strcmp(name, "acpi,sta") || !strcmp(name, "_STA")) {
        if (r->sta_error)
            return r->sta_error;
        data = &r->sta;
        size = sizeof(r->sta);
    } else if (!strcmp(name, "acpi,tpm2-start-method") && tpm2_method_valid &&
               record_matches(r, "MSFT0101")) {
        data = &tpm2_start_method;
        size = sizeof(tpm2_start_method);
    } else if (!strcmp(name, "acpi,i2c-address") && r->i2c_address) {
        data = &r->i2c_address;
        size = sizeof(r->i2c_address);
    } else if (!strcmp(name, "acpi,i2c-speed") && r->i2c_speed) {
        data = &r->i2c_speed;
        size = sizeof(r->i2c_speed);
    } else if (!strcmp(name, "acpi,hid-descriptor-register")) {
        if (r->hid_desc_error)
            return r->hid_desc_error;
        data = &r->hid_desc_register;
        size = sizeof(r->hid_desc_register);
    } else if (r->info) {
        if ((!strcmp(name, "acpi,hid") || !strcmp(name, "_HID") ||
             !strcmp(name, "hardware-id")) &&
            r->info->hid.value) {
            data = r->info->hid.value;
            size = r->info->hid.size;
        } else if (!strcmp(name, "acpi,cid") || !strcmp(name, "_CID")) {
            data = r->cid;
            size = r->cid_size;
        } else if ((!strcmp(name, "acpi,uid") || !strcmp(name, "_UID")) &&
                   r->info->uid.value) {
            data = r->info->uid.value;
            size = r->info->uid.size;
        } else if ((!strcmp(name, "acpi,adr") || !strcmp(name, "_ADR")) &&
                   (r->info->flags & UACPI_NS_NODE_INFO_HAS_ADR)) {
            data = &r->info->adr;
            size = sizeof(r->info->adr);
        } else if (!strcmp(name, "_CRS") && r->crs) {
            if (r->crs_error)
                return r->crs_error;
            data = r->crs->entries;
            size = r->crs->length;
        }
    }
    if (!data || !size)
        return -ENOENT;
    *out = (fw_property_t){data, size, FW_ORDER_NATIVE};
    return 0;
}
static int reference(const fw_node_t *n, const char *name, const char *cells,
                     size_t index, fw_reference_t *out) {
    acpi_node_record_t *r = record_for(n);
    if (!r || !name || !out)
        return -EINVAL;
    if (!cells || strcmp(cells, "#gpio-cells") || index ||
        strcmp(name, "acpi,gpio-attention") || !r->i2c_address || !r->gpio_attention)
        return -ENOENT;
    acpi_node_record_t *provider = record_find(r->gpio_attention);
    if (!provider || provider->sta_error || (provider->sta & 3U) != 3U)
        return -ENOENT;
    *out = (fw_reference_t){.node = node_for(provider), .count = 2,
                            .args = {r->gpio_pin, r->gpio_active_low}};
    return 0;
}
static int resource(const fw_node_t *n, size_t index, fw_resource_t *out) {
    acpi_node_record_t *r = record_for(n);
    if (!r || !out)
        return -EINVAL;
    if (r->crs_error)
        return r->crs_error;
    if (index >= r->resource_count)
        return -ENOENT;
    *out = r->resources[index];
    return 0;
}
static int irq(const fw_node_t *n, size_t index, fw_reference_t *out) {
    acpi_node_record_t *r = record_for(n);
    if (!r || !out)
        return -EINVAL;
    if (r->crs_error)
        return r->crs_error;
    if (index >= r->irq_count)
        return -ENOENT;
    *out =
        (fw_reference_t){.node = node_for(root_record),
                         .count = 2,
                         .args = {r->irqs[index].number, r->irqs[index].flags}};
    return 0;
}
static int available(const fw_node_t *n) {
    acpi_node_record_t *r = record_for(n);
    return !r ? -EINVAL : r->sta_error ? r->sta_error : (r->sta & 3U) == 3U;
}
static int children_available(const fw_node_t *n) {
    acpi_node_record_t *r = record_for(n);
    return !r ? -EINVAL : r->sta_error ? r->sta_error : !!(r->sta & (1U | 8U));
}
static int match_id(const fw_node_t *n, const char *id) {
    return record_matches(record_for(n), id);
}
static const fw_node_ops_t acpi_ops = {
    .source = "acpi",
    .name = node_name,
    .parent = parent,
    .next_child = next_child,
    .property = property,
    .reference = reference,
    .resource = resource,
    .irq = irq,
    .available = available,
    .children_available = children_available,
    .match_id = match_id,
};
int acpi_namespace_root(fw_node_t *out) {
    if (!out)
        return -EINVAL;
    if (namespace_state != 1)
        return -ENOENT;
    *out = node_for(root_record);
    return 0;
}
int acpi_namespace_initialize(const acpi_context_t *context, fw_node_t *out) {
    if (!context || !out)
        return -EINVAL;
    if (namespace_state)
        return namespace_state == 1 ? acpi_namespace_root(out) : -EIO;
    active_context = *context;
    acpi_table_t tpm2;
    if (!acpi_table_find(context, "TPM2", 0, &tpm2)) {
        uint64_t method;
        if (!acpi_table_read(&tpm2, 48, 4, &method)) {
            tpm2_start_method = (uint32_t)method;
            tpm2_method_valid = 1;
        }
        acpi_table_release(context, &tpm2);
    }
    namespace_state = -1;
    uacpi_status status = uacpi_initialize(UACPI_FLAG_BAD_CSUM_FATAL |
                                           UACPI_FLAG_BAD_TBL_SIGNATURE_FATAL |
                                           UACPI_FLAG_PROACTIVE_TBL_CSUM);
    if (!status)
        status = uacpi_namespace_load();
    if (!status)
        status = uacpi_set_interrupt_model(arch_acpi_interrupt_model());
    if (!status)
        status = uacpi_namespace_initialize();
    if (status) {
        printf("[ACPI] uACPI namespace unavailable: %s\n",
               uacpi_status_to_string(status));
        return error(status);
    }
    root_record = record_add(uacpi_namespace_root());
    if (!root_record)
        return snapshot_error;
    status = uacpi_namespace_for_each_child(uacpi_namespace_root(), snapshot,
                                            NULL, UACPI_OBJECT_DEVICE_BIT,
                                            UACPI_MAX_DEPTH_ANY, NULL);
    if (status || snapshot_error)
        return status ? error(status) : snapshot_error;
    namespace_state = 1;
    *out = node_for(root_record);
    printf("[ACPI] uACPI %u.%u.%u namespace ready\n", UACPI_MAJOR, UACPI_MINOR,
           UACPI_PATCH);
    return 0;
}
int acpi_pci_root(unsigned segment, unsigned bus, fw_node_t *out) {
    if (!out || namespace_state != 1)
        return -ENOENT;
    for (acpi_node_record_t *r = records; r; r = r->all_next) {
        fw_node_t n = node_for(r);
        if ((!match_id(&n, "PNP0A03") && !match_id(&n, "PNP0A08")) ||
            available(&n) != 1)
            continue;
        uacpi_u64 seg = 0, bbn = 0;
        uacpi_status a = uacpi_eval_simple_integer(r->handle, "_SEG", &seg);
        uacpi_status b = uacpi_eval_simple_integer(r->handle, "_BBN", &bbn);
        if ((a && a != UACPI_STATUS_NOT_FOUND) ||
            (b && b != UACPI_STATUS_NOT_FOUND))
            continue;
        if (seg == segment && bbn == bus) {
            *out = n;
            return 0;
        }
    }
    return -ENOENT;
}
int acpi_pci_child(const fw_node_t *n, unsigned dev, unsigned function,
                   fw_node_t *out) {
    acpi_node_record_t *p = record_for(n);
    if (!p || !out)
        return -EINVAL;
    uint64_t adr = (uint64_t)dev << 16 | function;
    for (acpi_node_record_t *r = p->first_child; r; r = r->next) {
        if (r->info && (r->info->flags & UACPI_NS_NODE_INFO_HAS_ADR) &&
            r->info->adr == adr) {
            *out = node_for(r);
            return 0;
        }
    }
    return -ENOENT;
}
struct link_irq {
    size_t index;
    uint32_t gsi;
    int found;
};
static uacpi_iteration_decision link_resource(void *opaque, uacpi_resource *r) {
    struct link_irq *s = opaque;
    if (s->index--)
        return UACPI_ITERATION_DECISION_CONTINUE;
    if (r->type == UACPI_RESOURCE_TYPE_IRQ && r->irq.num_irqs == 1) {
        s->gsi = r->irq.irqs[0];
        s->found = 1;
    } else if (r->type == UACPI_RESOURCE_TYPE_EXTENDED_IRQ &&
               r->extended_irq.num_irqs == 1 &&
               !r->extended_irq.source.length) {
        s->gsi = r->extended_irq.irqs[0];
        s->found = 1;
    }
    return UACPI_ITERATION_DECISION_BREAK;
}
int acpi_pci_route(const fw_node_t *n, unsigned dev, unsigned function,
                   unsigned pin, uint32_t *out) {
    acpi_node_record_t *r = record_for(n);
    if (!r || !out || dev > 31 || function > 7 || pin < 1 || pin > 4)
        return -EINVAL;
    uacpi_pci_routing_table *table;
    uacpi_status status = uacpi_get_pci_routing_table(r->handle, &table);
    if (status)
        return error(status);
    int rc = -ENOENT;
    for (size_t i = 0; i < table->num_entries; i++) {
        uacpi_pci_routing_table_entry *e = &table->entries[i];
        unsigned fn = e->address & 0xffffU;
        if ((e->address >> 16) != dev || (fn != 0xffffU && fn != function) ||
            e->pin != pin - 1)
            continue;
        if (!e->source) {
            *out = e->index;
            rc = 0;
            break;
        }
        uacpi_u32 sta;
        status = uacpi_eval_sta(e->source, &sta);
        if (status || (sta & 3U) != 3U) {
            rc = status ? error(status) : -ENOENT;
            break;
        }
        struct link_irq state = {.index = e->index};
        status = uacpi_for_each_device_resource(e->source, "_CRS",
                                                link_resource, &state);
        if (!status && state.found) {
            *out = state.gsi;
            rc = 0;
        } else
            rc = status ? error(status) : -EINVAL;
        break;
    }
    uacpi_free_pci_routing_table(table);
    return rc;
}
int acpi_namespace_poweroff(void) {
    if (namespace_state != 1)
        return -ENOENT;
    uacpi_status status = uacpi_prepare_for_sleep_state(UACPI_SLEEP_STATE_S5);
    printf("[ACPI-POWER] S5 prepare: %s\n", uacpi_status_to_string(status));
    if (status != UACPI_STATUS_OK)
        return error(status);
    printf("[ACPI-POWER] entering firmware S5\n");
    status = uacpi_enter_sleep_state(UACPI_SLEEP_STATE_S5);
    printf("[ACPI-POWER] S5 returned: %s\n", uacpi_status_to_string(status));
    return error(status);
}
int acpi_namespace_reboot(void) {
    if (namespace_state != 1)
        return -ENOENT;
    printf("[ACPI-POWER] requesting FADT reset\n");
    uacpi_status status = uacpi_reboot();
    printf("[ACPI-POWER] FADT reset returned: %s\n", uacpi_status_to_string(status));
    return error(status);
}
#else
int acpi_namespace_reboot(void) { return -EOPNOTSUPP; }
int acpi_namespace_initialize(const acpi_context_t *c, fw_node_t *n) {
    (void)c;
    (void)n;
    return -EOPNOTSUPP;
}
int acpi_namespace_root(fw_node_t *n) {
    (void)n;
    return -ENOENT;
}
int acpi_pci_root(unsigned s, unsigned b, fw_node_t *n) {
    (void)s;
    (void)b;
    (void)n;
    return -ENOENT;
}
int acpi_pci_child(const fw_node_t *n, unsigned d, unsigned f, fw_node_t *o) {
    (void)n;
    (void)d;
    (void)f;
    (void)o;
    return -ENOENT;
}
int acpi_pci_route(const fw_node_t *n, unsigned d, unsigned f, unsigned p,
                   uint32_t *g) {
    (void)n;
    (void)d;
    (void)f;
    (void)p;
    (void)g;
    return -ENOENT;
}
int acpi_namespace_poweroff(void) {
    return -EOPNOTSUPP;
}
#endif
