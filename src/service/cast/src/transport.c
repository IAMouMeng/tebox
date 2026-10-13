#include "cast/transport.h"
#include "endian_io.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_CLIENTS 16
/* A viewer this far behind (about 4 s at 8 Mbps) is dropped. */
#define MAX_PENDING (4u << 20)

struct cast_client {
    int fd;
    bool need_key;       /* joined mid-GOP: skip packets until a keyframe */
    uint8_t *buf;        /* bytes not yet accepted by the socket */
    size_t len, cap;
    uint8_t *in;         /* partial upstream record (camera / microphone) */
    size_t in_len, in_cap;
};

struct cast_transport {
    int listen_fd;
    bool is_listener;
    struct cast_client clients[MAX_CLIENTS];
    int nclients;
    cast_uplink_cb uplink;
    void *uplink_user;
};

struct cast_file_sink {
    FILE *fp;
};

static void set_nodelay(int fd)
{
    int one = 1;

    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

static void set_nosigpipe(int fd)
{
#ifdef SO_NOSIGPIPE
    int one = 1;

    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
    (void)fd;
#endif
}

static int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static struct cast_client *add_client(struct cast_transport *t, int fd,
                                      bool need_key)
{
    struct cast_client *c;

    if (t->nclients >= MAX_CLIENTS) {
        return NULL;
    }
    c = &t->clients[t->nclients++];
    memset(c, 0, sizeof(*c));
    c->fd = fd;
    c->need_key = need_key;
    set_nonblock(fd);
    set_nodelay(fd);
    set_nosigpipe(fd);
    return c;
}

static void drop_client(struct cast_transport *t, int i, const char *why)
{
    close(t->clients[i].fd);
    free(t->clients[i].buf);
    free(t->clients[i].in);
    t->clients[i] = t->clients[--t->nclients];
    fprintf(stderr, "cast-transport: client %s (%d left)\n", why, t->nclients);
}

/* Push buffered bytes without blocking. Returns false if the peer is gone. */
static bool flush_client(struct cast_client *c)
{
    size_t off = 0;

    while (off < c->len) {
        ssize_t n = send(c->fd, c->buf + off, c->len - off, MSG_DONTWAIT);

        if (n > 0) {
            off += (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        } else {
            return false;
        }
    }
    memmove(c->buf, c->buf + off, c->len - off);
    c->len -= off;
    return true;
}

static bool queue_bytes(struct cast_client *c, const uint8_t *data, size_t size)
{
    if (c->len + size > c->cap) {
        size_t ncap = (c->len + size) * 2;
        uint8_t *n = realloc(c->buf, ncap);

        if (!n) {
            return false;
        }
        c->buf = n;
        c->cap = ncap;
    }
    memcpy(c->buf + c->len, data, size);
    c->len += size;
    return true;
}

/* Drain what the viewer sent and hand complete records to the uplink
 * callback. Returns false if the viewer is gone or misbehaving. */
static bool read_client(struct cast_transport *t, struct cast_client *c)
{
    for (int round = 0; round < 64; round++) {
        size_t want = 4;
        ssize_t n;

        if (c->in_len >= 4) {
            uint32_t length = cast_r32be(c->in);

            if (length < CAST_ENC_HDR_SIZE || length > CAST_UPLINK_MAX) {
                return false;
            }
            want = 4 + length;
        }
        if (want > c->in_cap) {
            uint8_t *nb = realloc(c->in, want);

            if (!nb) {
                return false;
            }
            c->in = nb;
            c->in_cap = want;
        }
        n = recv(c->fd, c->in + c->in_len, want - c->in_len, MSG_DONTWAIT);
        if (n == 0) {
            return false;
        }
        if (n < 0) {
            return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
        }
        c->in_len += (size_t)n;
        if (c->in_len == want && want > 4) {
            if (t->uplink) {
                t->uplink(t->uplink_user, c->in[4], c->in + 4 + CAST_ENC_HDR_SIZE,
                          want - 4 - CAST_ENC_HDR_SIZE);
            }
            c->in_len = 0;
        }
    }
    return true;
}

void cast_transport_set_uplink(struct cast_transport *t, cast_uplink_cb cb,
                               void *user)
{
    if (t) {
        t->uplink = cb;
        t->uplink_user = user;
    }
}

struct cast_transport *cast_transport_listen(const char *host, int port)
{
    struct cast_transport *t = calloc(1, sizeof(*t));
    struct sockaddr_in addr;
    int yes = 1;

    if (!t) {
        return NULL;
    }
    t->is_listener = true;
    t->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (t->listen_fd < 0) {
        free(t);
        return NULL;
    }
    setsockopt(t->listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = host && host[0]
        ? inet_addr(host) : htonl(INADDR_ANY);
    if (bind(t->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(t->listen_fd, MAX_CLIENTS) < 0) {
        close(t->listen_fd);
        free(t);
        return NULL;
    }
    set_nonblock(t->listen_fd);
    fprintf(stderr, "cast-transport: listening on %s:%d\n",
            host && host[0] ? host : "0.0.0.0", port);
    return t;
}

struct cast_transport *cast_transport_connect(const char *host, int port)
{
    struct cast_transport *t = calloc(1, sizeof(*t));
    struct sockaddr_in addr;
    int fd;

    if (!t) {
        return NULL;
    }
    t->listen_fd = -1;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        free(t);
        return NULL;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = inet_addr(host && host[0] ? host : "127.0.0.1");
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        free(t);
        return NULL;
    }
    add_client(t, fd, false);
    return t;
}

void cast_transport_destroy(struct cast_transport *t)
{
    if (!t) {
        return;
    }
    while (t->nclients > 0) {
        close(t->clients[t->nclients - 1].fd);
        free(t->clients[t->nclients - 1].buf);
        free(t->clients[t->nclients - 1].in);
        t->nclients--;
    }
    if (t->listen_fd >= 0) {
        close(t->listen_fd);
    }
    free(t);
}

bool cast_transport_ready(const struct cast_transport *t)
{
    return t && t->nclients > 0;
}

int cast_transport_try_accept(struct cast_transport *t)
{
    bool accepted = false;

    if (!t || !t->is_listener || t->listen_fd < 0) {
        return -1;
    }
    /* Read upstream records; this also notices viewers that left, which a
     * static screen (no writes to fail) would otherwise hide. */
    for (int i = t->nclients - 1; i >= 0; i--) {
        if (!read_client(t, &t->clients[i]) || !flush_client(&t->clients[i])) {
            drop_client(t, i, "disconnected");
        }
    }
    for (;;) {
        int fd = accept(t->listen_fd, NULL, NULL);

        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                break;
            }
            return -1;
        }
        if (!add_client(t, fd, true)) {
            fprintf(stderr, "cast-transport: too many viewers, refusing one\n");
            close(fd);
            continue;
        }
        accepted = true;
        fprintf(stderr, "cast-transport: client connected (%d viewing)\n",
                t->nclients);
    }
    return accepted ? 2 : t->nclients > 0;
}

int cast_transport_send_packet(struct cast_transport *t,
                               enum cast_codec codec, uint8_t flags,
                               uint64_t pts_us,
                               const uint8_t *payload, size_t size)
{
    uint8_t hdr[4 + CAST_ENC_HDR_SIZE];
    int sent = 0;

    if (!t || !payload) {
        return -1;
    }
    cast_w32be(hdr, (uint32_t)(CAST_ENC_HDR_SIZE + size));
    hdr[4] = (uint8_t)codec;
    hdr[5] = flags;
    cast_w64be(hdr + 6, pts_us);

    for (int i = t->nclients - 1; i >= 0; i--) {
        struct cast_client *c = &t->clients[i];

        if (c->need_key && !(flags & CAST_FLAG_KEY)) {
            continue;
        }
        c->need_key = false;
        if (c->len + sizeof(hdr) + size > MAX_PENDING) {
            drop_client(t, i, "too slow, dropped");
            continue;
        }
        if (!queue_bytes(c, hdr, sizeof(hdr)) || !queue_bytes(c, payload, size) ||
            !flush_client(c)) {
            drop_client(t, i, "disconnected");
            continue;
        }
        sent++;
    }
    return sent > 0 ? 0 : -1;
}

struct cast_file_sink *cast_file_sink_open(const char *path)
{
    struct cast_file_sink *s = calloc(1, sizeof(*s));

    if (!s) {
        return NULL;
    }
    s->fp = fopen(path, "wb");
    if (!s->fp) {
        free(s);
        return NULL;
    }
    return s;
}

void cast_file_sink_close(struct cast_file_sink *s)
{
    if (!s) {
        return;
    }
    if (s->fp) {
        fclose(s->fp);
    }
    free(s);
}

int cast_file_sink_write(struct cast_file_sink *s,
                         const uint8_t *data, size_t size)
{
    if (!s || !s->fp || !data) {
        return -1;
    }
    return fwrite(data, 1, size, s->fp) == size ? 0 : -1;
}
