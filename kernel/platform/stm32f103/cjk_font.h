#ifndef _STM32F103_CJK_FONT_H
#define _STM32F103_CJK_FONT_H

#include "core/types.h"

/* Load /FONT/CJK16.FNT into external SRAM and register it with ui_render. */
int cjk_font_load(void);
void cjk_font_unload(void);
unsigned cjk_font_glyph_count(void);

#endif /* _STM32F103_CJK_FONT_H */
