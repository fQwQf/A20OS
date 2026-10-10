#ifndef A20_DRVMOD_EXPORT_H
#define A20_DRVMOD_EXPORT_H

struct drv_export {
    const char *name;
    void *addr;
};
extern const struct drv_export drv_export_table[];
extern const unsigned drv_export_count;
extern const struct drv_export arch_drv_export_table[];
extern const unsigned arch_drv_export_count;

#endif
