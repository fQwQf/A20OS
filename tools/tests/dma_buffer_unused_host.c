/* These page-only fixtures must never acquire buffer ownership. Production
 * allocation + mapping integration is exercised by test_driver_buffer_mapping. */
#include <assert.h>
#include "drivers/core/dma_buffer.h"
void driver_dma_buffer_get(struct driver_dma_buffer *buffer) { assert(!buffer); }
void driver_dma_buffer_put(struct driver_dma_buffer *buffer) { assert(!buffer); }
void *driver_dma_buffer_data(const struct driver_dma_buffer *buffer)
{ (void)buffer; assert(0); return 0; }
uint64_t driver_dma_buffer_address(const struct driver_dma_buffer *buffer)
{ (void)buffer; assert(0); return 0; }
size_t driver_dma_buffer_size(const struct driver_dma_buffer *buffer)
{ (void)buffer; assert(0); return 0; }
