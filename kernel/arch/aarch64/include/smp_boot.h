#ifndef _AARCH64_SMP_BOOT_H
#define _AARCH64_SMP_BOOT_H
/* Publish all levels of the current bootstrap page map to PoC before a
 * secondary with caches/translation off starts its page-table walk. */
void aarch64_boot_clean_page_tables(void);
#endif
