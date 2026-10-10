#include "cast/control.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

struct cast_control {
    int listen_fd;
    int conn_fd;
    cast_control_on_set_fn on_set;
    cast_control_on_stats_fn on_stats;
    void *user;
    char line[2048];
    size_t line_len;
};

static int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

struct cast_control *cast_control_listen(const char *host, int port,
                                         cast_control_on_set_fn on_set,
                                         cast_control_on_stats_fn on_stats,
                                         void *user)
{
    struct cast_control *c = calloc(1, sizeof(*c));
    struct sockaddr_in addr;
    int yes = 1;

    if (!c) {
        return NULL;
    }
    c->on_set = on_set;
    c->on_stats = on_stats;
    c->user = user;
    c->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    c->conn_fd = -1;
    if (c->listen_fd < 0) {
        free(c);
        return NULL;
    }
    setsockopt(c->listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = host && host[0]
        ? inet_addr(host) : htonl(INADDR_LOOPBACK);
    if (bind(c->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(c->listen_fd, 1) < 0) {
        close(c->listen_fd);
        free(c);
        return NULL;
    }
    set_nonblock(c->listen_fd);
    fprintf(stderr, "cast-control: listening on %s:%d\n",
            host && host[0] ? host : "127.0.0.1", port);
    return c;
}

void cast_control_destroy(struct cast_control *c)
{
    if (!c) {
        return;
    }
    if (c->conn_fd >= 0) {
        close(c->conn_fd);
    }
    if (c->listen_fd >= 0) {
        close(c->listen_fd);
    }
    free(c);
}

void cast_control_reply(struct cast_control *c, const char *json_line)
{
    char buf[2200];
    size_t n;

    if (!c || c->conn_fd < 0 || !json_line) {
        return;
    }
    n = (size_t)snprintf(buf, sizeof(buf), "%s\n", json_line);
    if (n >= sizeof(buf)) {
        n = sizeof(buf) - 1;
    }
    (void)send(c->conn_fd, buf, n, 0);
}

static long parse_long_field(const char *line, const char *key)
{
    const char *p = strstr(line, key);
    char *end;
    long v;

    if (!p) {
        return -1;
    }
    p += strlen(key);
    while (*p == ' ' || *p == ':' || *p == '"') {
        p++;
    }
    v = strtol(p, &end, 10);
    if (end == p) {
        return -1;
    }
    return v;
}

static void parse_max_size(const char *line, int *mw, int *mh)
{
    const char *p = strstr(line, "\"max_size\"");
    int a = 0, b = 0;

    if (!p) {
        return;
    }
    p = strchr(p, '[');
    if (!p) {
        return;
    }
    if (sscanf(p, "[%d,%d]", &a, &b) == 2) {
        *mw = a;
        *mh = b;
    }
}

static void parse_codec(const char *line, enum cast_codec *codec)
{
    const char *p = strstr(line, "\"codec\"");
    char name[32];

    if (!p) {
        return;
    }
    p = strchr(p + 7, '"');
    if (!p) {
        return;
    }
    p++;
    if (sscanf(p, "%31[^\"]", name) == 1) {
        *codec = cast_codec_from_name(name);
    }
}

static void handle_line(struct cast_control *c, const char *line)
{
    struct cast_encoder_cfg cfg;
    long v;
    bool force_only = false;

    memset(&cfg, 0, sizeof(cfg));
    /* width==-1 marks "codec not present in this command". */
    cfg.width = -1;
    cfg.codec = CAST_CODEC_H264;
    cfg.fps = -1;
    cfg.bitrate = -1;
    cfg.max_width = -1;
    cfg.max_height = -1;
    cfg.gop_sec = -1;

    if (strstr(line, "\"force_idr\"")) {
        force_only = true;
        if (c->on_set) {
            c->on_set(c->user, &cfg, true);
        }
        cast_control_reply(c, "{\"ok\":true,\"cmd\":\"force_idr\"}");
        return;
    }
    if (!strstr(line, "\"set\"") && !strstr(line, "\"cmd\":\"set\"")) {
        if (strstr(line, "\"get_stats\"") || strstr(line, "\"stats\"")) {
            char stats[512];

            snprintf(stats, sizeof(stats),
                     "{\"ok\":true,\"cmd\":\"get_stats\"}");
            if (c->on_stats) {
                c->on_stats(c->user, stats, sizeof(stats));
            }
            cast_control_reply(c, stats);
            return;
        }
        cast_control_reply(c, "{\"ok\":false,\"error\":\"unknown_cmd\"}");
        return;
    }

    v = parse_long_field(line, "\"bitrate\"");
    if (v > 0) {
        cfg.bitrate = v;
    }
    v = parse_long_field(line, "\"fps\"");
    if (v > 0) {
        cfg.fps = (int)v;
    }
    v = parse_long_field(line, "\"gop_sec\"");
    if (v > 0) {
        cfg.gop_sec = (int)v;
    }
    parse_max_size(line, &cfg.max_width, &cfg.max_height);
    if (strstr(line, "\"codec\"")) {
        parse_codec(line, &cfg.codec);
        cfg.width = 0; /* codec field present */
    }

    if (c->on_set) {
        c->on_set(c->user, &cfg, force_only);
    }
    cast_control_reply(c, "{\"ok\":true,\"cmd\":\"set\",\"reconfigured\":true}");
}

int cast_control_poll(struct cast_control *c)
{
    char buf[512];
    ssize_t n;
    size_t i;

    if (!c) {
        return -1;
    }
    if (c->conn_fd < 0) {
        int fd = accept(c->listen_fd, NULL, NULL);

        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;
            }
            return -1;
        }
        set_nonblock(fd);
        c->conn_fd = fd;
        c->line_len = 0;
        fprintf(stderr, "cast-control: client connected\n");
    }

    n = recv(c->conn_fd, buf, sizeof(buf), 0);
    if (n == 0) {
        close(c->conn_fd);
        c->conn_fd = -1;
        c->line_len = 0;
        return 0;
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        close(c->conn_fd);
        c->conn_fd = -1;
        return -1;
    }

    for (i = 0; i < (size_t)n; i++) {
        char ch = buf[i];

        if (ch == '\n' || ch == '\r') {
            if (c->line_len > 0) {
                c->line[c->line_len] = '\0';
                handle_line(c, c->line);
                c->line_len = 0;
            }
            continue;
        }
        if (c->line_len + 1 < sizeof(c->line)) {
            c->line[c->line_len++] = ch;
        }
    }
    return 0;
}
