/* Compile this translation unit with Clang -femulated-tls. The harness calls
 * actual compiler-generated TLS accessors, not a hand-constructed descriptor. */
static __thread unsigned initialized = 17;
static __thread unsigned char data[257] __attribute__((aligned(64)));
unsigned *probe_counter(void) { return &initialized; }
unsigned char *probe_data(void) { return data; }
