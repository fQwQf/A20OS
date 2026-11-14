/* These fixtures never map an owning VMO. Native factory/lifetime coverage
 * lives in test_driver_resident_mapping; acquiring one here is a model bug. */
#include <assert.h>
struct vmo;
void vmo_release(struct vmo *vmo) { assert(!vmo); }
