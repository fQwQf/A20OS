// SPDX-License-Identifier: GPL-2.0+
/* Board-only U-Boot handoff: publish actual probed scanout, never guessed RAM. */
#include <dm.h>
#include <event.h>
#include <fdt_support.h>
#include <video.h>
#include <linux/libfdt.h>

static int zero_simplefb_fixup(void *ctx, struct event *event)
{
    void *fdt = event->data.ft_fixup.tree.fdt;
    struct udevice *dev;
    struct video_priv *video;
    struct video_uc_plat *plat;
    const char *format;
    int chosen, node, ret;

    if (!video_is_active())
        return 0; /* Headless boot remains a valid serial-shell boot. */
    ret = uclass_first_device_err(UCLASS_VIDEO, &dev);
    if (ret)
        return ret;
    video = dev_get_uclass_priv(dev);
    plat = dev_get_uclass_plat(dev);
    if (!video->xsize || !video->ysize || video->bpix != VIDEO_BPP32 ||
        video->line_length != video->xsize * 4 ||
        (u64)video->line_length * video->ysize > plat->size)
        return -EINVAL;
    if (video->format == VIDEO_X8R8G8B8)
        format = "x8r8g8b8";
    else if (video->format == VIDEO_X8B8G8R8)
        format = "x8b8g8r8";
    else
        return -EINVAL;

    /* Reserve the whole U-Boot video allocation, not only visible pixels. */
    ret = fdt_add_fb_mem_rsv(fdt);
    if (ret)
        return ret;
    chosen = fdt_path_offset(fdt, "/chosen");
    if (chosen < 0)
        return chosen;
    ret = fdt_setprop_u32(fdt, chosen, "#address-cells", 2);
    if (ret)
        return ret;
    ret = fdt_setprop_u32(fdt, chosen, "#size-cells", 2);
    if (ret)
        return ret;
    ret = fdt_setprop(fdt, chosen, "ranges", NULL, 0);
    if (ret)
        return ret;
    node = fdt_add_subnode(fdt, chosen, "framebuffer");
    if (node < 0)
        return node;
    ret = fdt_setprop_string(fdt, node, "compatible", "simple-framebuffer");
    if (ret)
        return ret;
    ret = fdt_setup_simplefb_node(fdt, node, plat->base, video->xsize,
                               video->ysize, video->line_length, format);
    if (ret)
        return ret;
    /* No clock/power teardown: VIDEO_REMOVE is disabled in this profile. */
    printf("A20OS HDMI simplefb: %ux%u stride=%u base=%lx\n",
           video->xsize, video->ysize, video->line_length, plat->base);
    return 0;
}
EVENT_SPY_FULL(EVT_FT_FIXUP, zero_simplefb_fixup);
