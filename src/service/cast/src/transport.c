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

static void set_nodelay(int fd)
{
    int one = 1;

    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

struct cast_transport {
    int listen_fd;
    int conn_fd;
    bool is_listener;
};

struct cast_file_sink {
    FILE *fp;
};

static int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

struct cast_transport *cast_transport_listen(const char *host, int port)
{
    struct cast_transport *t = calloc(1, sizeof(*t));
    struct sockaddr_in addr;
    int yes = 1;

    if (!t) {
        return NULL;
    }
    t->listen_fd = -1;
    t->conn_fd = -1;
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
        listen(t->listen_fd, 1) < 0) {
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

    if (!t) {
        return NULL;
    }
    t->listen_fd = -1;
    t->conn_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (t->conn_fd < 0) {
        free(t);
        return NULL;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = inet_addr(host && host[0] ? host : "127.0.0.1");
    if (connect(t->conn_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(t->conn_fd);
        free(t);
        return NULL;
    }
#ifdef SO_NOSIGPIPE
    {
        int one = 1;
        setsockopt(t->conn_fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    }
#endif
    return t;
}

void cast_transport_destroy(struct cast_transport *t)
{
    if (!t) {
        return;
    }
    if (t->conn_fd >= 0) {
        close(t->conn_fd);
    }
    if (t->listen_fd >= 0) {
        close(t->listen_fd);
    }
    free(t);
}

bool cast_transport_ready(const struct cast_transport *t)
{
    return t && t->conn_fd >= 0;
}

int cast_transport_fd(const struct cast_transport *t)
{
    return t ? t->conn_fd : -1;
}

int cast_transport_try_accept(struct cast_transport *t)
{
    int fd;

    if (!t || !t->is_listener || t->listen_fd < 0) {
        return -1;
    }
    if (t->conn_fd >= 0) {
        return 1;
    }
    fd = accept(t->listen_fd, NULL, NULL);
    if (fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        return -1;
    }
    t->conn_fd = fd;
    set_nodelay(fd);
#ifdef SO_NOSIGPIPE
    {
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    }
#endif
    fprintf(stderr, "cast-transport: client connected\n");
    return 1;
}

int cast_transport_send_packet(struct cast_transport *t,
                               enum cast_codec codec, uint8_t flags,
                               uint64_t pts_us,
                               const uint8_t *payload, size_t size)
{
    uint8_t hdr[4 + CAST_ENC_HDR_SIZE];
    uint32_t length;
    size_t off = 0;
    ssize_t n;

    if (!t || t->conn_fd < 0 || !payload) {
        return -1;
    }
    length = (uint32_t)(CAST_ENC_HDR_SIZE + size);
    cast_w32be(hdr, length);
    hdr[4] = (uint8_t)codec;
    hdr[5] = flags;
    cast_w64be(hdr + 6, pts_us);

    while (off < sizeof(hdr)) {
        n = send(t->conn_fd, hdr + off, sizeof(hdr) - off, 0);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                continue;
            }
            close(t->conn_fd);
            t->conn_fd = -1;
            return -1;
        }
        off += (size_t)n;
    }
    off = 0;
    while (off < size) {
        n = send(t->conn_fd, payload + off, size - off, 0);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                continue;
            }
            close(t->conn_fd);
            t->conn_fd = -1;
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
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
