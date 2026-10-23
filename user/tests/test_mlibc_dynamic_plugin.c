/*
 * Dynamic-link plugin for the Native mlibc runtime smoke.
 *
 * Loaded with dlopen() by test_mlibc_dynamic; exercises per-thread TLS in a
 * shared object (global-dynamic TLS through __tls_get_addr) and a plain
 * cross-object call.
 */
static _Thread_local int plug_tls;

int plug_answer(void)
{
    return 42;
}

void plug_tls_set(int value)
{
    plug_tls = value;
}

int plug_tls_get(void)
{
    return plug_tls;
}
