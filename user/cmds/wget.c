#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <netinet/in.h>

#include "tlse.h"

#define DNS_SERVER_IP 0x0302000aU
#define MAX_REDIRECTS 4
#define READ_TIMEOUT_MS 10000
#define CONNECT_TIMEOUT_MS 10000

typedef struct {
    char host[256];
    char path[768];
    uint16_t port;
    int https;
} http_url_t;

typedef struct {
    int fd;
    struct TLSContext *tls;
} net_conn_t;

static uint16_t bswap16(uint16_t x) {
    return (uint16_t)((x << 8) | (x >> 8));
}

static long now_ms(void) {
    struct timeval tv;
    if (gettimeofday(&tv, NULL) < 0)
        return 0;
    return tv.tv_sec * 1000L + tv.tv_usec / 1000L;
}

static uint32_t parse_ipv4(const char *s) {
    unsigned a, b, c, d;
    char tail;
    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4)
        return 0;
    if (a > 255 || b > 255 || c > 255 || d > 255)
        return 0;
    return (uint32_t)(a | (b << 8) | (c << 16) | (d << 24));
}

static int dns_skip_name(const uint8_t *msg, size_t len, size_t *off) {
    size_t p = *off;
    while (p < len) {
        uint8_t c = msg[p++];
        if (c == 0) {
            *off = p;
            return 0;
        }
        if ((c & 0xc0) == 0xc0) {
            if (p >= len)
                return -1;
            *off = p + 1;
            return 0;
        }
        if ((c & 0xc0) != 0 || p + c > len)
            return -1;
        p += c;
    }
    return -1;
}

static int dns_encode_name(uint8_t *buf, size_t bufsz, size_t *off, const char *name) {
    const char *p = name;
    while (*p) {
        const char *dot = strchr(p, '.');
        size_t labellen = dot ? (size_t)(dot - p) : strlen(p);
        if (labellen == 0 || labellen > 63 || *off + 1 + labellen >= bufsz)
            return -1;
        buf[(*off)++] = (uint8_t)labellen;
        memcpy(buf + *off, p, labellen);
        *off += labellen;
        if (!dot)
            break;
        p = dot + 1;
    }
    if (*off >= bufsz)
        return -1;
    buf[(*off)++] = 0;
    return 0;
}

static int resolve_a_record(const char *name, uint32_t *out_addr) {
    uint32_t literal = parse_ipv4(name);
    if (literal) {
        *out_addr = literal;
        return 0;
    }

    uint8_t query[512];
    memset(query, 0, sizeof(query));
    uint16_t txid = (uint16_t)(0x5700U ^ (uint16_t)getpid());
    query[0] = (uint8_t)(txid >> 8);
    query[1] = (uint8_t)txid;
    query[2] = 0x01;
    query[5] = 0x01;

    size_t qlen = 12;
    if (dns_encode_name(query, sizeof(query), &qlen, name) < 0 || qlen + 4 > sizeof(query))
        return -1;
    query[qlen++] = 0x00;
    query[qlen++] = 0x01;
    query[qlen++] = 0x00;
    query[qlen++] = 0x01;

    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_in dns;
    memset(&dns, 0, sizeof(dns));
    dns.sin_family = AF_INET;
    dns.sin_addr.s_addr = DNS_SERVER_IP;
    dns.sin_port = bswap16(53);

    long start = now_ms();
    if (sendto(fd, query, qlen, 0, (struct sockaddr *)&dns, sizeof(dns)) < 0) {
        close(fd);
        return -1;
    }

    uint8_t reply[512];
    ssize_t n = -1;
    while (now_ms() - start < 3000) {
        n = recvfrom(fd, reply, sizeof(reply), 0, NULL, NULL);
        if (n >= 0)
            break;
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            close(fd);
            return -1;
        }
        usleep(10000);
    }
    close(fd);

    if (n < 12 || reply[0] != query[0] || reply[1] != query[1])
        return -1;
    if ((reply[3] & 0x0f) != 0)
        return -1;

    uint16_t qd = (uint16_t)((reply[4] << 8) | reply[5]);
    uint16_t an = (uint16_t)((reply[6] << 8) | reply[7]);
    size_t off = 12;
    for (uint16_t i = 0; i < qd; i++) {
        if (dns_skip_name(reply, (size_t)n, &off) < 0 || off + 4 > (size_t)n)
            return -1;
        off += 4;
    }
    for (uint16_t i = 0; i < an; i++) {
        if (dns_skip_name(reply, (size_t)n, &off) < 0 || off + 10 > (size_t)n)
            return -1;
        uint16_t type = (uint16_t)((reply[off] << 8) | reply[off + 1]);
        uint16_t class = (uint16_t)((reply[off + 2] << 8) | reply[off + 3]);
        uint16_t rdlen = (uint16_t)((reply[off + 8] << 8) | reply[off + 9]);
        off += 10;
        if (off + rdlen > (size_t)n)
            return -1;
        if (type == 1 && class == 1 && rdlen == 4) {
            memcpy(out_addr, reply + off, 4);
            return 0;
        }
        off += rdlen;
    }
    return -1;
}

static int parse_http_url(const char *url, http_url_t *out) {
    const char *p = url;
    memset(out, 0, sizeof(*out));
    if (strncmp(p, "http://", 7) == 0) {
        out->https = 0;
        out->port = 80;
        p += 7;
    } else if (strncmp(p, "https://", 8) == 0) {
        out->https = 1;
        out->port = 443;
        p += 8;
    } else {
        return -1;
    }
    if (*p == '\0' || *p == '/')
        return -1;

    const char *slash = strchr(p, '/');
    const char *host_end = slash ? slash : p + strlen(p);
    const char *colon = NULL;
    for (const char *q = p; q < host_end; q++) {
        if (*q == ':') {
            colon = q;
            break;
        }
    }

    size_t host_len = (size_t)((colon ? colon : host_end) - p);
    if (host_len == 0 || host_len >= sizeof(out->host))
        return -1;
    memcpy(out->host, p, host_len);
    out->host[host_len] = '\0';

    if (colon) {
        char portbuf[8];
        size_t port_len = (size_t)(host_end - colon - 1);
        if (port_len == 0 || port_len >= sizeof(portbuf))
            return -1;
        memcpy(portbuf, colon + 1, port_len);
        portbuf[port_len] = '\0';
        int port = atoi(portbuf);
        if (port <= 0 || port > 65535)
            return -1;
        out->port = (uint16_t)port;
    }

    if (slash) {
        if (strlen(slash) >= sizeof(out->path))
            return -1;
        strcpy(out->path, slash);
    } else {
        strcpy(out->path, "/");
    }
    return 0;
}

static const char *default_output_name(const http_url_t *url, char out[256]) {
    const char *end = url->path + strlen(url->path);
    const char *q = strchr(url->path, '?');
    if (q)
        end = q;
    const char *slash = end;
    while (slash > url->path && slash[-1] != '/')
        slash--;
    if (slash == end)
        return "index.html";
    size_t len = (size_t)(end - slash);
    if (len >= 256)
        len = 255;
    memcpy(out, slash, len);
    out[len] = '\0';
    return out;
}

static int send_plain_all(int fd, const unsigned char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, buf + off, len - off, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(10000);
                continue;
            }
            return -1;
        }
        if (n == 0)
            return -1;
        off += (size_t)n;
    }
    return 0;
}

static int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int tls_flush(net_conn_t *conn) {
    unsigned int out_len = 0;
    const unsigned char *out = tls_get_write_buffer(conn->tls, &out_len);
    if (out && out_len) {
        if (send_plain_all(conn->fd, out, out_len) < 0)
            return -1;
        tls_buffer_clear(conn->tls);
    }
    return 0;
}

static int connect_tcp(const http_url_t *url) {
    uint32_t addr;
    if (resolve_a_record(url->host, &addr) < 0) {
        fprintf(stderr, "wget: cannot resolve %s\n", url->host);
        return -1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = addr;
    dst.sin_port = bswap16(url->port);
    if (connect(fd, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }
    return fd;
}

static int connect_url(const http_url_t *url, net_conn_t *conn) {
    memset(conn, 0, sizeof(*conn));
    conn->fd = connect_tcp(url);
    if (conn->fd < 0)
        return -1;

    if (!url->https)
        return 0;

    if (set_nonblock(conn->fd) < 0) {
        perror("fcntl");
        close(conn->fd);
        return -1;
    }

    conn->tls = tls_create_context(0, TLS_V12);
    if (!conn->tls) {
        fprintf(stderr, "wget: cannot create TLS context\n");
        close(conn->fd);
        return -1;
    }
    tls_sni_set(conn->tls, url->host);
    if (tls_client_connect(conn->tls) < 0 || tls_flush(conn) < 0) {
        fprintf(stderr, "wget: TLS handshake failed\n");
        tls_destroy_context(conn->tls);
        close(conn->fd);
        return -1;
    }

    unsigned char buf[2048];
    long start = now_ms();
    while (tls_established(conn->tls) == 0) {
        ssize_t n = recv(conn->fd, buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (now_ms() - start >= CONNECT_TIMEOUT_MS) {
                    fprintf(stderr, "wget: TLS handshake timed out\n");
                    tls_destroy_context(conn->tls);
                    close(conn->fd);
                    return -1;
                }
                usleep(10000);
                continue;
            }
            perror("recv");
            tls_destroy_context(conn->tls);
            close(conn->fd);
            return -1;
        }
        if (n == 0) {
            fprintf(stderr, "wget: TLS connection closed during handshake\n");
            tls_destroy_context(conn->tls);
            close(conn->fd);
            return -1;
        }
        int r = tls_consume_stream(conn->tls, buf, (int)n, NULL);
        if (r < 0 || tls_flush(conn) < 0) {
            fprintf(stderr, "wget: TLS handshake failed (%d)\n", r);
            tls_destroy_context(conn->tls);
            close(conn->fd);
            return -1;
        }
    }

    if (tls_established(conn->tls) < 0) {
        fprintf(stderr, "wget: TLS handshake failed\n");
        tls_destroy_context(conn->tls);
        close(conn->fd);
        return -1;
    }
    return 0;
}

static void conn_close(net_conn_t *conn) {
    if (conn->tls) {
        tls_close_notify(conn->tls);
        tls_flush(conn);
        tls_destroy_context(conn->tls);
        conn->tls = NULL;
    }
    if (conn->fd >= 0) {
        close(conn->fd);
        conn->fd = -1;
    }
}

static int conn_send_all(net_conn_t *conn, const char *buf, size_t len) {
    if (!conn->tls)
        return send_plain_all(conn->fd, (const unsigned char *)buf, len);

    size_t off = 0;
    while (off < len) {
        size_t chunk = len - off;
        if (chunk > 4096)
            chunk = 4096;
        int r = tls_write(conn->tls, (const unsigned char *)buf + off, (unsigned int)chunk);
        if (r < 0 || tls_flush(conn) < 0)
            return -1;
        off += chunk;
    }
    return 0;
}

static ssize_t conn_recv(net_conn_t *conn, char *buf, size_t len) {
    long start = now_ms();
    if (!conn->tls) {
        for (;;) {
            ssize_t n = recv(conn->fd, buf, len, 0);
            if (n >= 0)
                return n;
            if (errno == EINTR)
                continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                return -1;
            if (now_ms() - start >= READ_TIMEOUT_MS) {
                errno = ETIMEDOUT;
                return -1;
            }
            usleep(10000);
        }
    }

    for (;;) {
        int r = tls_read(conn->tls, (unsigned char *)buf, (unsigned int)len);
        if (r > 0)
            return r;
        if (r < 0) {
            errno = EIO;
            return -1;
        }

        unsigned char enc[16384];
        ssize_t n = recv(conn->fd, enc, sizeof(enc), 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (now_ms() - start >= READ_TIMEOUT_MS) {
                    errno = ETIMEDOUT;
                    return -1;
                }
                usleep(10000);
                continue;
            }
            return -1;
        }
        if (n == 0)
            return 0;
        r = tls_consume_stream(conn->tls, enc, (int)n, NULL);
        if (tls_flush(conn) < 0) {
            int saved_errno = errno;
            int buffered = tls_read(conn->tls, (unsigned char *)buf, (unsigned int)len);
            if (buffered > 0)
                return buffered;
            errno = saved_errno ? saved_errno : EIO;
            return -1;
        }
        if (r < 0) {
            r = tls_read(conn->tls, (unsigned char *)buf, (unsigned int)len);
            if (r > 0)
                return r;
            return 0;
        }
    }
}

static int header_get_location(const char *header, char *out, size_t outsz) {
    const char *p = header;
    while (*p) {
        const char *eol = strstr(p, "\r\n");
        if (!eol)
            break;
        size_t len = (size_t)(eol - p);
        if (len >= 10 && strncasecmp(p, "Location:", 9) == 0) {
            p += 9;
            while (*p == ' ' || *p == '\t')
                p++;
            len = (size_t)(eol - p);
            if (len >= outsz)
                return -1;
            memcpy(out, p, len);
            out[len] = '\0';
            return 0;
        }
        p = eol + 2;
    }
    return -1;
}

static int fetch_once(const char *urlstr, const char *output_arg, char *redirect, size_t redirect_sz) {
    http_url_t url;
    char normalized_url[1024];
    if (parse_http_url(urlstr, &url) < 0) {
        if (strstr(urlstr, "://")) {
            fprintf(stderr, "wget: only http:// or https:// URLs are supported\n");
            return 1;
        }
        if (strlen(urlstr) + strlen("http://") >= sizeof(normalized_url)) {
            fprintf(stderr, "wget: URL too long\n");
            return 1;
        }
        snprintf(normalized_url, sizeof(normalized_url), "http://%s", urlstr);
        if (parse_http_url(normalized_url, &url) < 0) {
            fprintf(stderr, "wget: invalid URL\n");
            return 1;
        }
    }

    net_conn_t conn;
    if (connect_url(&url, &conn) < 0)
        return 1;

    char req[1400];
    int default_port = url.https ? 443 : 80;
    int req_len = snprintf(req, sizeof(req),
                           "GET %s HTTP/1.1\r\n"
                           "Host: %s%s%u\r\n"
                           "User-Agent: A20OS-wget/0.1\r\n"
                           "Connection: close\r\n\r\n",
                           url.path, url.host,
                           url.port == default_port ? "" : ":",
                           url.port == default_port ? 0 : url.port);
    if (url.port == default_port) {
        req_len = snprintf(req, sizeof(req),
                           "GET %s HTTP/1.1\r\n"
                           "Host: %s\r\n"
                           "User-Agent: A20OS-wget/0.1\r\n"
                           "Connection: close\r\n\r\n",
                           url.path, url.host);
    }
    if (req_len < 0 || (size_t)req_len >= sizeof(req) ||
        conn_send_all(&conn, req, (size_t)req_len) < 0) {
        perror("send");
        conn_close(&conn);
        return 1;
    }
    set_nonblock(conn.fd);

    char outname_buf[256];
    const char *outname = output_arg ? output_arg : default_output_name(&url, outname_buf);
    int outfd = STDOUT_FILENO;
    if (strcmp(outname, "-") != 0) {
        outfd = open(outname, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (outfd < 0) {
            perror(outname);
            conn_close(&conn);
            return 1;
        }
    }

    char buf[1024];
    char header[4096];
    size_t header_len = 0;
    int header_done = 0;
    int status = 0;
    size_t total = 0;

    for (;;) {
        ssize_t n = conn_recv(&conn, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("recv");
            if (outfd != STDOUT_FILENO)
                close(outfd);
            conn_close(&conn);
            return 1;
        }
        if (n == 0)
            break;

        char *body = buf;
        size_t body_len = (size_t)n;
        if (!header_done) {
            if (header_len + (size_t)n >= sizeof(header)) {
                fprintf(stderr, "wget: response header too large\n");
                if (outfd != STDOUT_FILENO)
                    close(outfd);
                conn_close(&conn);
                return 1;
            }
            memcpy(header + header_len, buf, (size_t)n);
            header_len += (size_t)n;
            header[header_len] = '\0';
            char *sep = strstr(header, "\r\n\r\n");
            if (!sep)
                continue;
            header_done = 1;
            sscanf(header, "HTTP/%*s %d", &status);
            if (status >= 300 && status < 400 &&
                header_get_location(header, redirect, redirect_sz) == 0) {
                if (outfd != STDOUT_FILENO)
                    close(outfd);
                conn_close(&conn);
                return 2;
            }
            if (status < 200 || status >= 300) {
                fprintf(stderr, "wget: server returned HTTP %d\n", status);
                if (outfd != STDOUT_FILENO)
                    close(outfd);
                conn_close(&conn);
                return 1;
            }
            body = sep + 4;
            body_len = header_len - (size_t)(body - header);
        }

        if (body_len > 0) {
            ssize_t w = write(outfd, body, body_len);
            if (w < 0 || (size_t)w != body_len) {
                perror("write");
                if (outfd != STDOUT_FILENO)
                    close(outfd);
                conn_close(&conn);
                return 1;
            }
            total += body_len;
        }
    }

    if (outfd != STDOUT_FILENO)
        close(outfd);
    conn_close(&conn);
    if (strcmp(outname, "-") != 0)
        fprintf(stderr, "%s: saved %zu bytes\n", outname, total);
    return 0;
}

int main(int argc, char **argv) {
    const char *output = NULL;
    const char *url = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-O") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "usage: wget [-O file|-] URL\n");
                return 1;
            }
            output = argv[i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("usage: wget [-O file|-] URL\n");
            return 0;
        } else if (!url) {
            url = argv[i];
        } else {
            fprintf(stderr, "wget: extra operand: %s\n", argv[i]);
            return 1;
        }
    }

    if (!url) {
        fprintf(stderr, "usage: wget [-O file|-] URL\n");
        return 1;
    }

    char current[1024];
    if (strlen(url) >= sizeof(current)) {
        fprintf(stderr, "wget: URL too long\n");
        return 1;
    }
    strcpy(current, url);

    for (int i = 0; i <= MAX_REDIRECTS; i++) {
        char redirect[1024] = {0};
        int r = fetch_once(current, output, redirect, sizeof(redirect));
        if (r != 2)
            return r;
        if (strncmp(redirect, "http://", 7) != 0 &&
            strncmp(redirect, "https://", 8) != 0) {
            fprintf(stderr, "wget: unsupported redirect: %s\n", redirect);
            return 1;
        }
        if (strlen(redirect) >= sizeof(current)) {
            fprintf(stderr, "wget: redirect URL too long\n");
            return 1;
        }
        strcpy(current, redirect);
    }

    fprintf(stderr, "wget: too many redirects\n");
    return 1;
}
