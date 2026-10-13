#ifndef A20_NATIVE_SPAWN_PARAMS_H
#define A20_NATIVE_SPAWN_PARAMS_H
/* Identical v1/v2 header admission for full VM and flat execution domains. */
static inline int a20_spawn_copy_params(const a20_task_spawn_args_t *user,
                                        a20_task_spawn_args_t *out) {
    if (!user)
        return -A20_ERR_FAULT;
    uint32_t header[2];
    if (copy_from_user(header, user, sizeof(header)) < 0)
        return -A20_ERR_FAULT;
    if (header[1] < 1 || header[1] > 2)
        return -A20_ERR_INVALID_ARGUMENT;
    uint32_t minimum =
        header[1] == 1 ? A20_TASK_SPAWN_ARGS_V1_SIZE : sizeof(*out);
    if (header[0] < minimum)
        return -A20_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->stdin_handle = out->stdout_handle = out->stderr_handle =
        A20_HANDLE_NULL;
    uint32_t size = header[0] < sizeof(*out) ? header[0] : sizeof(*out);
    if (copy_from_user(out, user, size) < 0)
        return -A20_ERR_FAULT;
    out->version = header[1];
    if ((out->flags & ~A20_SPAWN_PARENT_BOUND) ||
        (out->version == 2 && out->reserved))
        return -A20_ERR_INVALID_ARGUMENT;
    return A20_OK;
}
#endif
