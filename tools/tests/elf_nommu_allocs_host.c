#include <assert.h>
#include "mm/elf_nommu_allocs.h"

static int released[4];
static int release_count;

static void fake_release(void *ptr)
{
    int id = (int)(long)ptr;
    assert(id > 0 && id < 4);
    assert(!released[id]);
    released[id] = 1;
    release_count++;
}

int main(void)
{
    void *temporary[4] = {(void *)1, (void *)2, (void *)3, NULL};
    size_t sizes[4] = {10, 20, 30, 0};
    uint8_t types[4] = {1, 2, 3, 0};
    int temporary_count = 3;
    void *owned[4] = {0};
    size_t owned_sizes[4] = {0};
    uint8_t owned_types[4] = {0};
    int owned_count = 0;

    /* Successful load transfers ownership; temp cleanup must do nothing. */
    elf_nommu_allocs_transfer(owned, owned_sizes, owned_types, &owned_count,
                              temporary, sizes, types, &temporary_count, 4);
    assert(temporary_count == 0 && owned_count == 3);
    assert(temporary[0] == NULL && temporary[1] == NULL && temporary[2] == NULL);
    elf_nommu_allocs_release(temporary, sizes, types, &temporary_count, 4,
                             fake_release);
    assert(release_count == 0);

    /* The receiving owner releases each tracked block exactly once. */
    elf_nommu_allocs_release(owned, owned_sizes, owned_types, &owned_count, 4,
                             fake_release);
    assert(release_count == 3);
    assert(owned_count == 0);
    for (int i = 0; i < 4; i++)
        assert(owned[i] == NULL && owned_sizes[i] == 0 && owned_types[i] == 0);
    elf_nommu_allocs_release(owned, owned_sizes, owned_types, &owned_count, 4,
                             fake_release);
    assert(release_count == 3);

    /* Failed-load cleanup releases temp ownership and is idempotent too. */
    for (int i = 0; i < 4; i++) released[i] = 0;
    release_count = 0;
    temporary[0] = (void *)1;
    temporary[1] = (void *)2;
    temporary[2] = (void *)3;
    sizes[0] = 10; sizes[1] = 20; sizes[2] = 30;
    types[0] = 1; types[1] = 2; types[2] = 3;
    temporary_count = 3;
    elf_nommu_allocs_release(temporary, sizes, types, &temporary_count, 4,
                             fake_release);
    assert(release_count == 3);
    assert(temporary_count == 0);
    return 0;
}
