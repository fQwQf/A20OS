/* Only an explicit firmware selftest fixture. Production media is supplied by
 * a20_flat_files_bind's sector callbacks, independent of this platform. */
#ifdef CONFIG_NATIVE_FLAT_FILES
#include "fs/flat_files.h"
#include "flat-file-fixture.h"
static int fixture_read(void *ctx, uint32_t lba, void *buffer, uint32_t count) {
    (void)ctx;
    if (lba >= FLAT_FIXTURE_SECTORS || count > FLAT_FIXTURE_SECTORS - lba)
        return -1;
    uint8_t *out = buffer;
    for (uint32_t sector = 0; sector < count; sector++) {
        memset(out + sector * 512, 0, 512);
        for (unsigned i = 0; i < FLAT_FIXTURE_RECORDS; i++)
            if (flat_fixture_lba[i] == lba + sector) {
                memcpy(out + sector * 512, flat_fixture_data[i], 512);
                break;
            }
    }
    return 0;
}
static void program_file_test_bind(void) {
    fat32lite_io_t io = {.read = fixture_read};
    CHECK(!a20_flat_files_bind(&io, 0));
}
#endif
