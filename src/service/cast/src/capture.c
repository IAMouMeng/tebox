#include "cast/capture.h"
#include "endian_io.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

struct cast_capture {
    int listen_fd;
    int conn_fd;
    char path[512];
    uint8_t hdrbuf[sizeof(struct cast_raw_header)];
    size_t hdr_got;
    struct cast_raw_header hdr;
    bool hdr_ready;
    uint8_t *body;
    size_t body_got;
    size_t body_need;
};

static int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

struct cast_capture *cast_capture_listen_unix(const char *path)
{
    struct cast_capture *cap;
    struct sockaddr_un addr;

    if (!path || !path[0] || strlen(path) >= sizeof(addr.sun_path)) {
        return NULL;
    }
    cap = calloc(1, sizeof(*cap));
    if (!cap) {
        return NULL;
    }
    snprintf(cap->path, sizeof(cap->path), "%s", path);
    unlink(path);
    cap->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    cap->conn_fd = -1;
    if (cap->listen_fd < 0) {
        free(cap);
        return NULL;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (bind(cap->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(cap->listen_fd, 1) < 0) {
        close(cap->listen_fd);
        free(cap);
        return NULL;
    }
    set_nonblock(cap->listen_fd);
    fprintf(stderr, "cast-capture: listening on %s\n", path);
    return cap;
}

void cast_capture_destroy(struct cast_capture *cap)
{
    if (!cap) {
        return;
    }
    if (cap->conn_fd >= 0) {
        close(cap->conn_fd);
    }
    if (cap->listen_fd >= 0) {
        close(cap->listen_fd);
    }
    free(cap->body);
    if (cap->path[0]) {
        unlink(cap->path);
    }
    free(cap);
}

void cast_frame_free(struct cast_frame *f)
{
    if (!f) {
        return;
    }
    free(f->data);
    memset(f, 0, sizeof(*f));
}

static void reset_frame_state(struct cast_capture *cap)
{
    free(cap->body);
    cap->body = NULL;
    cap->hdr_got = 0;
    cap->hdr_ready = false;
    cap->body_got = 0;
    cap->body_need = 0;
}

static int try_accept(struct cast_capture *cap)
{
    int fd;

    if (cap->conn_fd >= 0) {
        return 0;
    }
    fd = accept(cap->listen_fd, NULL, NULL);
    if (fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        return -1;
    }
    set_nonblock(fd);
    {
        /* Keep receive buffer small so backlog cannot grow into seconds of lag. */
        int rcvbuf = 2 * 1024 * 1024;
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    }
    cap->conn_fd = fd;
    reset_frame_state(cap);
    fprintf(stderr, "cast-capture: QEMU connected\n");
    return 0;
}

static int parse_header(struct cast_capture *cap)
{
    const uint8_t *p = cap->hdrbuf;

    cap->hdr.magic = cast_r32be(p);
    cap->hdr.version = cast_r32be(p + 4);
    cap->hdr.width = cast_r32be(p + 8);
    cap->hdr.height = cast_r32be(p + 12);
    cap->hdr.stride = cast_r32be(p + 16);
    cap->hdr.format = cast_r32be(p + 20);
    cap->hdr.pts_us = cast_r64be(p + 24);
    cap->hdr.nbytes = cast_r32be(p + 32);

    if (cap->hdr.magic != CAST_RAW_MAGIC ||
        cap->hdr.version != CAST_RAW_VERSION ||
        cap->hdr.format != CAST_FMT_BGRA8888 ||
        cap->hdr.width == 0 || cap->hdr.height == 0 ||
        cap->hdr.nbytes == 0 ||
        cap->hdr.nbytes > 64u * 1024u * 1024u) {
        fprintf(stderr, "cast-capture: bad frame header\n");
        return -1;
    }
    cap->body_need = cap->hdr.nbytes;
    cap->body = malloc(cap->body_need);
    if (!cap->body) {
        return -1;
    }
    cap->body_got = 0;
    cap->hdr_ready = true;
    return 0;
}

int cast_capture_poll_frame(struct cast_capture *cap, struct cast_frame *out)
{
    ssize_t n;

    memset(out, 0, sizeof(*out));
    if (!cap) {
        return -1;
    }
    if (try_accept(cap) < 0) {
        return -1;
    }
    if (cap->conn_fd < 0) {
        return 0;
    }

    if (!cap->hdr_ready) {
        n = recv(cap->conn_fd, cap->hdrbuf + cap->hdr_got,
                 sizeof(cap->hdrbuf) - cap->hdr_got, 0);
        if (n == 0) {
            close(cap->conn_fd);
            cap->conn_fd = -1;
            reset_frame_state(cap);
            return 0;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;
            }
            close(cap->conn_fd);
            cap->conn_fd = -1;
            reset_frame_state(cap);
            return -1;
        }
        cap->hdr_got += (size_t)n;
        if (cap->hdr_got < sizeof(cap->hdrbuf)) {
            return 0;
        }
        if (parse_header(cap) < 0) {
            close(cap->conn_fd);
            cap->conn_fd = -1;
            reset_frame_state(cap);
            return -1;
        }
    }

    n = recv(cap->conn_fd, cap->body + cap->body_got,
             cap->body_need - cap->body_got, 0);
    if (n == 0) {
        close(cap->conn_fd);
        cap->conn_fd = -1;
        reset_frame_state(cap);
        return 0;
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        close(cap->conn_fd);
        cap->conn_fd = -1;
        reset_frame_state(cap);
        return -1;
    }
    cap->body_got += (size_t)n;
    if (cap->body_got < cap->body_need) {
        return 0;
    }

    out->width = cap->hdr.width;
    out->height = cap->hdr.height;
    out->stride = cap->hdr.stride;
    out->format = cap->hdr.format;
    out->pts_us = cap->hdr.pts_us;
    out->data = cap->body;
    out->size = cap->body_need;
    cap->body = NULL;
    reset_frame_state(cap);
    return 1;
}
