/* SPDX-License-Identifier: MIT
 * RK3566 VP0, one XRGB plane. Register facts cross-checked with Linux v6.12
 * rockchip_drm_vop2.{c,h}: vop2_setup_alpha()/vop2_parse_alpha().
 * MIX0 at 0x650 + (mixer_id 0 + zpos 0 - 1)*0x10 is the bottom mixer.
 * Global alpha=255, pixel alpha disabled; source factor=global alpha,
 * destination factor=inverse source alpha. Thus C_out=C_src for every X byte.
 */
#ifndef A20_VOP2_OPAQUE_H
#define A20_VOP2_OPAQUE_H
#define A20_VOP2_BOTTOM_SRC_COLOR 0x640U
#define A20_VOP2_BOTTOM_DST_COLOR 0x644U
#define A20_VOP2_BOTTOM_SRC_ALPHA 0x648U
#define A20_VOP2_BOTTOM_DST_ALPHA 0x64cU
#define A20_VOP2_HDR_SRC_COLOR    0x6c0U
/* alpha_en bit8; color_mode NO_PRE_MUL bit0; blend GLOBAL bits3:2=0;
 * source factor SRC_GLOBAL bits7:5=5; global alpha bits23:16=255. */
#define A20_VOP2_OPAQUE_SRC_COLOR 0x00ff01a1U
/* Destination PRE_MUL, factor SRC_INVERSE=3, global alpha=255. */
#define A20_VOP2_OPAQUE_DST_COLOR 0x00ff0060U
#define A20_VOP2_OPAQUE_SRC_ALPHA 0x00000020U /* factor ONE */
#define A20_VOP2_OPAQUE_DST_ALPHA 0x00000078U /* PER_PIX_GLOBAL, no-saturation, inverse */
static inline void a20_vop2_opaque(void *regs)
{
    unsigned char *base = regs;
    writel(A20_VOP2_OPAQUE_SRC_COLOR, base + A20_VOP2_BOTTOM_SRC_COLOR);
    writel(A20_VOP2_OPAQUE_DST_COLOR, base + A20_VOP2_BOTTOM_DST_COLOR);
    writel(A20_VOP2_OPAQUE_SRC_ALPHA, base + A20_VOP2_BOTTOM_SRC_ALPHA);
    writel(A20_VOP2_OPAQUE_DST_ALPHA, base + A20_VOP2_BOTTOM_DST_ALPHA);
    /* vop2_setup_alpha(): no bottom-layer global-alpha effect => HDR bypass. */
    writel(0, base + A20_VOP2_HDR_SRC_COLOR);
}
#endif
