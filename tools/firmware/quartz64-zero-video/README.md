# Quartz64-Zero video firmware inputs

This optional U-Boot profile uses U-Boot v2026.07, commit
`ece349ade2973e220f524ce59e59711cc919263f`, plus the nine patches in
[Patchwork series 514476](https://patchwork.ozlabs.org/project/uboot/list/?series=514476),
submitted by Daniel Golle on 2026-07-19. VOP2/HDMI authors: Dang Huynh and
Daniel Golle. `series.json` records patch URLs and SHA256 digests; patch files
preserve authorship and Signed-off-by metadata. The patches modify GPL-2.0+
U-Boot code; newly added driver files retain their SPDX GPL-2.0+ headers.
They are firmware source inputs, not linked into the A20OS kernel.

`a20_simplefb.c` is A20OS board-specific U-Boot glue, GPL-2.0+.
`rk3566-quartz64-zero-video.dtsi` is dual GPL-2.0+/MIT. It uses the Zero v0.1
schematic, rather than copying Model A regulators: HDMI analog supply is
RK817 LDO2 at 0.9 V and LDO7 at 1.8 V (sheet 23, R2137/R2136).

The video allocation and actual mode are published during EVT_FT_FIXUP into
`/chosen/framebuffer@...`, with `/reserved-memory` protecting the entire
allocation. `VIDEO_REMOVE` is disabled so VOP2 remains running. A failed HDMI
probe publishes no framebuffer and leaves serial boot available. No guessed
framebuffer address, static resolution or invented hardware validation is used.
This profile also describes I2S1_M0 and RK817 headphone PCM for the native
A20OS driver. LDO4 supplies their shared 3.3 V IO domain; LDO2/LDO7 remain
the separate HDMI rails. U-Boot does not play audio. HDMI/I2S0 PCM is disabled.

`a20_vop2_opaque.h` (MIT) explicitly configures VP0's one-plane bottom
mixer for global alpha 255 with per-pixel alpha disabled, then bypasses HDR
blending. Register facts follow Linux v6.12 `vop2_setup_alpha` and
`vop2_parse_alpha`, including `MIX0_SRC_COLOR_CTRL + (0 - 1)*0x10` at 0x640.
The source factor uses global alpha and the destination factor uses its
inverse. Thus the X byte cannot change the output color; warm-boot alpha
register state is not trusted. This is why the handoff publishes x8r8g8b8
although the VOP fetch format is named ARGB8888.
