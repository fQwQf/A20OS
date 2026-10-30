/* Host test for the backend registry and capability contract.
 *
 * The registry is deliberately independent of any protocol headers, so it can
 * be exercised on the host without lwIP or FreeBSD:
 *
 *   cc -I kernel/include -I kernel tools/tests/net_backend_host.c \
 *      kernel/net/core/backend.c -o /tmp/net_backend_host && \
 *      /tmp/net_backend_host
 *
 * It pins the properties the kernel relies on: unknown selection is -ENODEV
 * (never a fallback), a capability without its module is rejected, duplicate
 * registration is -EEXIST, and the capability line is renderable.
 */
#include "net/backend.h"
#include "core/errno.h"
#include "core/stdio.h"
#include "core/string.h"

static int stub_open(struct net_socket *s) {
    (void)s;
    return 0;
}
static void stub_close(struct net_socket *s) {
    (void)s;
}
static int stub_bind(struct net_socket *s, const void *a, size_t n) {
    (void)s;
    (void)a;
    (void)n;
    return 0;
}
static int stub_connect(struct net_socket *s, const void *a, size_t n,
                        const void *p, size_t pn) {
    (void)s;
    (void)a;
    (void)n;
    (void)p;
    (void)pn;
    return 0;
}
static int stub_send(struct net_socket *s, const void *b, size_t n, int f,
                     const void *a, size_t an) {
    (void)s;
    (void)b;
    (void)n;
    (void)f;
    (void)a;
    (void)an;
    return (int)n;
}
static int stub_listen(struct net_socket *s, int backlog) {
    (void)s;
    (void)backlog;
    return 0;
}

static void stub_init(void) {
}

static int stub_validate(struct net_socket *s, int level, int opt,
                         const void *v, size_t n) {
    (void)s;
    (void)level;
    (void)opt;
    (void)v;
    (void)n;
    return 0;
}

static const net_socket_opt_ops_t stub_opts = {.validate = stub_validate};
static const net_backend_cycle_ops_t stub_cycle = {.init = stub_init};
static const net_backend_event_ops_t stub_event = {0};
static const net_socket_ops_t stub_socket = {
    .open = stub_open,
    .close = stub_close,
    .bind = stub_bind,
    .connect = stub_connect,
    .send = stub_send,
    .listen = stub_listen,
};

static const net_backend_t stub_backend = {
    .name = "stub",
    .caps = NET_CAP_STREAM | NET_CAP_LISTEN,
    .cycle = &stub_cycle,
    .event = &stub_event,
    .socket = &stub_socket,
    .opts = &stub_opts,
};

static int failures;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            failures++;                                                        \
        }                                                                      \
    } while (0)

int main(void) {
    char caps[128];

    CHECK(net_backend_current() == NULL);
    CHECK(net_backend_select("missing") == -ENODEV);
    CHECK(net_backend_current() == NULL);

    /* LISTEN without socket->listen: the descriptor is inconsistent. */
    net_backend_t broken = stub_backend;
    broken.name = "broken";
    broken.socket = &(net_socket_ops_t){0};
    CHECK(net_backend_register(&broken) == -EINVAL);

    CHECK(net_backend_register(&stub_backend) == 0);
    CHECK(net_backend_register(&stub_backend) == -EEXIST);

    CHECK(net_backend_select("stub") == 0);
    CHECK(net_backend_current() == &stub_backend);
    CHECK(net_backend_has(NET_CAP_STREAM));
    CHECK(net_backend_has(NET_CAP_STREAM | NET_CAP_LISTEN));
    CHECK(!net_backend_has(NET_CAP_TUN));

    CHECK(net_backend_format_caps(caps, sizeof(caps)) > 0);
    CHECK(strstr(caps, "stub:") != NULL);
    CHECK(strstr(caps, "stream") != NULL);
    CHECK(strstr(caps, "listen") != NULL);

    printf("NET_BACKEND_HOST: %s (%d failure%s)\n", failures ? "FAIL" : "PASS",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
