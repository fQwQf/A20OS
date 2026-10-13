#ifndef A20_CORE_RUNTIME_H
#define A20_CORE_RUNTIME_H
#include "uapi/a20/runtime.h"
/* One successful binding per kernel boot. Exact retry is idempotent; a
 * conflicting binding returns -EBUSY. Caller supplies checked authority. */
int a20_runtime_bind(const a20_runtime_binding_t *binding);
void a20_runtime_snapshot(a20_runtime_info_t *out);
/* Zero when unbound; fills only the namespace part on success. */
int a20_runtime_object_namespace(a20_object_id_t *out);
#endif
