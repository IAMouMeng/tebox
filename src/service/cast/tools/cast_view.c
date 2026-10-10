/* Low-latency live viewer: length-prefixed TCP → bundled ffplay stdin. */
#include "cast/protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int read_full(int fd, void *buf, size_t n)
{
    uint8_t *p = buf;
    size_t off = 0;

    while (off < n) {
        ssize_t r = recv(fd, p + off, n - off, 0);
        if (r == 0) {
            return 0;
        }
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        off += (size_t)r;
    }
    return 1;
}

static uint32_t r32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static const char *find_ffplay(char *buf, size_t buflen)
{
    const char *env = getenv("TEBOX_FFPLAY");
    const char *root = getenv("TEBOX_ROOT");

    if (env && env[0] && access(env, X_OK) == 0) {
        return env;
    }
    if (root && root[0]) {
        snprintf(buf, buflen, "%s/prebuilts/host/ffmpeg/bin/ffplay", root);
        if (access(buf, X_OK) == 0) {
            return buf;
        }
        snprintf(buf, buflen, "%s/out/cast-ffmpeg/bin/ffplay", root);
        if (access(buf, X_OK) == 0) {
            return buf;
        }
    }
    snprintf(buf, buflen, "ffplay");
    return buf;
}

int main(int argc, char **argv)
{
    const char *host = "127.0.0.1";
    int port = 9901;
    const char *vf = NULL;
    char ffplay_path[1024];
    const char *ffplay;
    int sock;
    struct sockaddr_in addr;
    int one = 1;
    FILE *pipe_fp;
    char cmd[1536];
    uint8_t *body = NULL;
    size_t body_cap = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--host") && i + 1 < argc) {
            host = argv[++i];
        } else if (!strcmp(argv[i], "--port") && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--vf") && i + 1 < argc) {
            vf = argv[++i];
        } else if (!strcmp(argv[i], "--help")) {
            fprintf(stderr,
                    "Usage: %s [--host H] [--port P] [--vf FILTER]\n"
                    "Reads tebox-cast TCP stream and plays via bundled ffplay.\n",
                    argv[0]);
            return 0;
        }
    }

    ffplay = find_ffplay(ffplay_path, sizeof(ffplay_path));
    if (vf && vf[0]) {
        snprintf(cmd, sizeof(cmd),
                 "\"%s\" -hide_banner -loglevel warning "
                 "-fflags nobuffer+flush_packets+discardcorrupt "
                 "-flags low_delay -framedrop "
                 "-probesize 32 -analyzeduration 0 -sync ext "
                 "-window_title 'tebox-cast LIVE' -vf \"%s\" -f h264 -",
                 ffplay, vf);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "\"%s\" -hide_banner -loglevel warning "
                 "-fflags nobuffer+flush_packets+discardcorrupt "
                 "-flags low_delay -framedrop "
                 "-probesize 32 -analyzeduration 0 -sync ext "
                 "-window_title 'tebox-cast LIVE' -f h264 -",
                 ffplay);
    }

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = inet_addr(host);
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(sock);
        return 1;
    }
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    fprintf(stderr, "cast-view: %s:%d → %s\n", host, port, ffplay);
    pipe_fp = popen(cmd, "w");
    if (!pipe_fp) {
        perror("popen ffplay");
        close(sock);
        return 1;
    }

    for (;;) {
        uint8_t lenbuf[4];
        uint32_t length;
        int got = read_full(sock, lenbuf, 4);

        if (got <= 0) {
            break;
        }
        length = r32be(lenbuf);
        if (length < CAST_ENC_HDR_SIZE || length > 32u * 1024u * 1024u) {
            fprintf(stderr, "cast-view: bad length %u\n", length);
            break;
        }
        if (length > body_cap) {
            uint8_t *nbuf = realloc(body, length);
            if (!nbuf) {
                break;
            }
            body = nbuf;
            body_cap = length;
        }
        got = read_full(sock, body, length);
        if (got <= 0) {
            break;
        }
        /* skip codec/flags/pts (10 bytes), write Annex-B / codec payload */
        if (fwrite(body + CAST_ENC_HDR_SIZE, 1,
                   length - CAST_ENC_HDR_SIZE, pipe_fp) !=
            length - CAST_ENC_HDR_SIZE) {
            break;
        }
        fflush(pipe_fp);
    }

    free(body);
    pclose(pipe_fp);
    close(sock);
    return 0;
}
