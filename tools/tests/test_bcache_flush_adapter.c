#include <assert.h>
#include <stdint.h>
#include "drivers/block/block_dev.h"

int32_t a20_ubc_flush_device(void *dev, int32_t *has_flush);

static int result;
static int flush(struct block_dev *dev) { (void)dev; return result; }

int main(void)
{
    block_dev_t dev = {0};
    int32_t has_flush = -1;
    assert(a20_ubc_flush_device(&dev, &has_flush) == -95);
    assert(has_flush == 0);
    dev.flush = flush;
    result = -95;
    assert(a20_ubc_flush_device(&dev, &has_flush) == -95); /* real callback error */
    assert(has_flush == 1);
    result = -5;
    assert(a20_ubc_flush_device(&dev, &has_flush) == -5);
    result = 0;
    assert(a20_ubc_flush_device(&dev, &has_flush) == 0);
    result = 1;
    assert(a20_ubc_flush_device(&dev, &has_flush) == -5); /* positive errno normalization */
    return 0;
}
