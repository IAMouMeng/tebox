/* Live cast viewer with input: tebox-cast TCP stream → libavcodec → SDL2.
 *
 * With --id the viewer goes through tebox-api: video from the cast endpoint
 * ("TEBOX-CAST/1 <id>"), input as JSON datagrams to the shared UDP input
 * endpoint (each event carries "id" and "seq"). Guest clipboard changes come
 * back as datagrams on the same socket. Without --id it reads a bare
 * tebox-cast port (view only). Video reconnects if the API restarts.
 */
#include <SDL.h>
#include <libavcodec/avcodec.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* tebox-cast encoded stream (src/service/cast/include/cast/protocol.h):
 * [u32 be length][u8 codec][u8 flags][u64 be pts_us][payload]. */
#define CAST_ENC_HDR_SIZE 10u
#define CAST_CODEC_H264   0

struct viewer {
    const char *host;
    int port;
    const char *id;
    int video_fd;
    int ctrl_fd;          /* connected UDP socket to the input endpoint */
    uint64_t seq;
    SDL_mutex *lock;
    AVFrame *latest;      /* decoded, owned under lock */
    bool fresh;
    bool quit;
    int frame_w, frame_h;
    uint64_t frames;
};

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

static int write_full(int fd, const char *buf, size_t n)
{
    size_t off = 0;

    while (off < n) {
        ssize_t w = send(fd, buf + off, n - off, 0);
        if (w < 0 && errno == EINTR) {
            continue;
        }
        if (w <= 0) {
            return -1;
        }
        off += (size_t)w;
    }
    return 0;
}

static int dial(const char *host, int port)
{
    struct addrinfo hints = {0}, *res = NULL, *ai;
    char service[16];
    int fd = -1, one = 1;

    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(service, sizeof(service), "%d", port);
    if (getaddrinfo(host, service, &hints, &res) != 0) {
        return -1;
    }
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd >= 0) {
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    }
    return fd;
}

static int dial_handshake(const char *host, int port, const char *proto,
                          const char *id)
{
    char line[256];
    int fd = dial(host, port);

    if (fd < 0 || !id) {
        return fd;
    }
    snprintf(line, sizeof(line), "%s %s\n", proto, id);
    if (write_full(fd, line, strlen(line)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static uint32_t r32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* Network + decode thread; hands the newest picture to the UI thread. */
static int video_thread(void *opaque)
{
    struct viewer *v = opaque;
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext *ctx = codec ? avcodec_alloc_context3(codec) : NULL;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    uint8_t *body = NULL;
    size_t cap = 0;

    if (!ctx || !pkt || !frame) {
        fprintf(stderr, "cast-view: no H.264 decoder\n");
        goto out;
    }
    ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx->thread_count = 1; /* frame threads add a frame of latency each */
    if (avcodec_open2(ctx, codec, NULL) < 0) {
        goto out;
    }
    while (!v->quit) {
        uint8_t lenbuf[4];
        uint32_t length;

        if (v->video_fd < 0 || read_full(v->video_fd, lenbuf, 4) <= 0) {
            /* API or cast restarted: keep the last picture and reconnect. */
            if (v->video_fd >= 0) {
                close(v->video_fd);
                fprintf(stderr, "cast-view: video lost, reconnecting\n");
            }
            SDL_Delay(1000);
            v->video_fd = dial_handshake(v->host, v->port, "TEBOX-CAST/1", v->id);
            avcodec_flush_buffers(ctx);
            continue;
        }
        length = r32be(lenbuf);
        if (length < CAST_ENC_HDR_SIZE || length > 32u * 1024u * 1024u) {
            fprintf(stderr, "cast-view: bad length %u\n", length);
            close(v->video_fd);
            v->video_fd = -1;
            continue;
        }
        if (length + AV_INPUT_BUFFER_PADDING_SIZE > cap) {
            uint8_t *nbuf = realloc(body, length + AV_INPUT_BUFFER_PADDING_SIZE);

            if (!nbuf) {
                break;
            }
            body = nbuf;
            cap = length + AV_INPUT_BUFFER_PADDING_SIZE;
        }
        if (read_full(v->video_fd, body, length) <= 0) {
            close(v->video_fd);
            v->video_fd = -1;
            continue;
        }
        if (body[0] != CAST_CODEC_H264) {
            continue;
        }
        memset(body + length, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        pkt->data = body + CAST_ENC_HDR_SIZE;
        pkt->size = (int)(length - CAST_ENC_HDR_SIZE);
        if (avcodec_send_packet(ctx, pkt) < 0) {
            continue;
        }
        while (avcodec_receive_frame(ctx, frame) == 0) {
            if (v->frames == 0) {
                fprintf(stderr, "cast-view: first frame %dx%d\n",
                        frame->width, frame->height);
            }
            SDL_LockMutex(v->lock);
            av_frame_unref(v->latest);
            av_frame_move_ref(v->latest, frame);
            v->fresh = true;
            v->frames++;
            SDL_UnlockMutex(v->lock);
        }
    }
out:
    free(body);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    v->quit = true;
    {
        SDL_Event ev = { .type = SDL_QUIT };
        SDL_PushEvent(&ev);
    }
    return 0;
}

/* One event per datagram; json is an object literal starting with '{'.
 * Send errors (API restarting) are ignored: UDP has no connection to lose. */
static void send_ctrl(struct viewer *v, const char *json)
{
    size_t n = strlen(json) + strlen(v->id) + 64;
    char *dgram;

    if (v->ctrl_fd < 0 || json[0] != '{' || !(dgram = malloc(n))) {
        return;
    }
    n = (size_t)snprintf(dgram, n, "{\"id\":\"%s\",\"seq\":%llu,%s\n", v->id,
                         (unsigned long long)++v->seq, json + 1);
    if (n > 60000) {
        fprintf(stderr, "cast-view: event too large for UDP, dropped\n");
    } else {
        (void)send(v->ctrl_fd, dgram, n, 0);
    }
    free(dgram);
}

static int open_input_udp(const char *host, int port)
{
    struct addrinfo hints = {0}, *res = NULL;
    char service[16];
    int fd = -1;

    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    snprintf(service, sizeof(service), "%d", port);
    if (getaddrinfo(host, service, &hints, &res) != 0) {
        return -1;
    }
    fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd >= 0 && connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

/* JSON string literal (with quotes) for UTF-8 text; caller frees. */
static char *json_quote(const char *text)
{
    size_t n = strlen(text);
    char *out = malloc(n * 6 + 3);
    char *p = out;

    if (!out) {
        return NULL;
    }
    *p++ = '"';
    for (; *text; text++) {
        unsigned char c = (unsigned char)*text;

        if (c == '"' || c == '\\') {
            *p++ = '\\';
            *p++ = (char)c;
        } else if (c == '\n') {
            *p++ = '\\';
            *p++ = 'n';
        } else if (c < 0x20) {
            p += sprintf(p, "\\u%04x", c);
        } else {
            *p++ = (char)c;
        }
    }
    *p++ = '"';
    *p = '\0';
    return out;
}

static void send_text_event(struct viewer *v, const char *type,
                            const char *text, const char *extra)
{
    char *quoted = json_quote(text);
    char *json;

    if (!quoted) {
        return;
    }
    json = malloc(strlen(quoted) + 64);
    if (json) {
        sprintf(json, "{\"type\":\"%s\",\"text\":%s%s}", type, quoted, extra);
        send_ctrl(v, json);
        free(json);
    }
    free(quoted);
}

static void send_keycode(struct viewer *v, int keycode, int meta)
{
    char json[96];

    snprintf(json, sizeof(json),
             "{\"type\":\"keycode\",\"keycode\":%d,\"meta\":%d}",
             keycode, meta);
    send_ctrl(v, json);
}

/* Decode the "text" string of a {"type":"clipboard",...} line; caller frees. */
static char *parse_clipboard_line(const char *line)
{
    const char *p;
    char *out, *o;

    if (!strstr(line, "\"type\":\"clipboard\"") ||
        !(p = strstr(line, "\"text\":\""))) {
        return NULL;
    }
    p += 8;
    out = o = malloc(strlen(p) + 1);
    if (!out) {
        return NULL;
    }
    while (*p && *p != '"') {
        unsigned cp;

        if (*p != '\\') {
            *o++ = *p++;
            continue;
        }
        p++;
        switch (*p) {
        case 'n': *o++ = '\n'; p++; continue;
        case 't': *o++ = '\t'; p++; continue;
        case 'r': *o++ = '\r'; p++; continue;
        case 'b': *o++ = '\b'; p++; continue;
        case 'f': *o++ = '\f'; p++; continue;
        case 'u': break;
        case '\0': continue;
        default: *o++ = *p++; continue;
        }
        if (sscanf(p + 1, "%4x", &cp) != 1) {
            break;
        }
        p += 5;
        if (cp >= 0xd800 && cp < 0xdc00 && p[0] == '\\' && p[1] == 'u') {
            unsigned lo;

            if (sscanf(p + 2, "%4x", &lo) == 1 && lo >= 0xdc00 && lo < 0xe000) {
                cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                p += 6;
            }
        }
        if (cp < 0x80) {
            *o++ = (char)cp;
        } else if (cp < 0x800) {
            *o++ = (char)(0xc0 | cp >> 6);
            *o++ = (char)(0x80 | (cp & 0x3f));
        } else if (cp < 0x10000) {
            *o++ = (char)(0xe0 | cp >> 12);
            *o++ = (char)(0x80 | ((cp >> 6) & 0x3f));
            *o++ = (char)(0x80 | (cp & 0x3f));
        } else {
            *o++ = (char)(0xf0 | cp >> 18);
            *o++ = (char)(0x80 | ((cp >> 12) & 0x3f));
            *o++ = (char)(0x80 | ((cp >> 6) & 0x3f));
            *o++ = (char)(0x80 | (cp & 0x3f));
        }
    }
    *o = '\0';
    return out;
}

static Uint32 clipboard_event_type;

/* Input-socket reader: guest clipboard changes become SDL user events so
 * the host clipboard is written on the main (Cocoa) thread. */
static int control_thread(void *opaque)
{
    struct viewer *v = opaque;
    int fd = v->ctrl_fd;
    char *line = NULL;
    size_t len = 0, cap = 0;

    while (!v->quit) {
        static char buf[65536];
        ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);

        if (n <= 0) {
            /* ECONNREFUSED while the API is down; keep listening. */
            SDL_Delay(200);
            continue;
        }
        if (buf[n - 1] != '\n') {
            buf[n++] = '\n';
        }
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] != '\n') {
                if (len + 2 > cap) {
                    char *nl = realloc(line, cap ? cap * 2 : 4096);

                    if (!nl) {
                        goto out;
                    }
                    line = nl;
                    cap = cap ? cap * 2 : 4096;
                }
                line[len++] = buf[i];
                continue;
            }
            line[len] = '\0';
            len = 0;
            char *text = parse_clipboard_line(line);
            if (text) {
                SDL_Event ev = { .type = clipboard_event_type };

                ev.user.data1 = text;
                SDL_PushEvent(&ev);
            } else if (strstr(line, "\"ok\":false")) {
                fprintf(stderr, "cast-view: %s\n", line);
            }
        }
    }
out:
    free(line);
    return 0;
}

static void send_touch(struct viewer *v, const char *action, float x, float y)
{
    char json[128];

    snprintf(json, sizeof(json),
             "{\"type\":\"touch\",\"action\":\"%s\",\"x\":%.5f,\"y\":%.5f}",
             action, x, y);
    send_ctrl(v, json);
}

static void send_key(struct viewer *v, const char *key)
{
    char json[96];

    snprintf(json, sizeof(json), "{\"type\":\"key\",\"key\":\"%s\"}", key);
    send_ctrl(v, json);
}

/* Letterboxed picture rectangle inside the window, in window points. */
static SDL_Rect fit_rect(SDL_Window *win, int fw, int fh)
{
    int ww, wh;
    SDL_Rect r;

    SDL_GetWindowSize(win, &ww, &wh);
    if (fw <= 0 || fh <= 0) {
        r.x = r.y = 0;
        r.w = ww;
        r.h = wh;
        return r;
    }
    if ((int64_t)ww * fh > (int64_t)wh * fw) {
        r.h = wh;
        r.w = (int)((int64_t)wh * fw / fh);
    } else {
        r.w = ww;
        r.h = (int)((int64_t)ww * fh / fw);
    }
    r.x = (ww - r.w) / 2;
    r.y = (wh - r.h) / 2;
    return r;
}

/* Cursor position in window points. Event coordinates are not used: on
 * Retina, sdl2-compat (SDL3) reports them in pixels while SDL2 used points,
 * so the global cursor minus the window origin is the only stable source. */
static void cursor_in_window(SDL_Window *win, int *x, int *y)
{
    int gx, gy, wx, wy;

    SDL_GetGlobalMouseState(&gx, &gy);
    SDL_GetWindowPosition(win, &wx, &wy);
    *x = gx - wx;
    *y = gy - wy;
}

static bool to_guest(struct viewer *v, SDL_Window *win, float *gx, float *gy)
{
    SDL_Rect r = fit_rect(win, v->frame_w, v->frame_h);
    int x, y;

    cursor_in_window(win, &x, &y);

    if (r.w <= 0 || r.h <= 0) {
        return false;
    }
    *gx = (float)(x - r.x) / (float)r.w;
    *gy = (float)(y - r.y) / (float)r.h;
    return *gx >= 0.f && *gx <= 1.f && *gy >= 0.f && *gy <= 1.f;
}

/* Android navigation keys, sent through QEMU's virtio keyboard. */
static const char *nav_key_for(SDL_Keycode sym)
{
    switch (sym) {
    case SDLK_ESCAPE:   return "back";
    case SDLK_HOME:
    case SDLK_F1:       return "home";
    case SDLK_F2:       return "menu";
    case SDLK_F5:       return "power";
    case SDLK_PAGEUP:   return "volume_up";
    case SDLK_PAGEDOWN: return "volume_down";
    default:            return NULL;
    }
}

/* Editing keys as Android KEYCODE_* values (text arrives as SDL_TEXTINPUT). */
static int android_keycode_for(SDL_Keycode sym)
{
    switch (sym) {
    case SDLK_BACKSPACE: return 67;  /* DEL */
    case SDLK_DELETE:    return 112; /* FORWARD_DEL */
    case SDLK_RETURN:
    case SDLK_KP_ENTER:  return 66;  /* ENTER */
    case SDLK_TAB:       return 61;
    case SDLK_UP:        return 19;
    case SDLK_DOWN:      return 20;
    case SDLK_LEFT:      return 21;
    case SDLK_RIGHT:     return 22;
    case SDLK_END:       return 123; /* MOVE_END */
    default:             return 0;
    }
}

/* Cmd/Ctrl shortcuts mapped to Android Ctrl+key (Cmd+V pastes the host
 * clipboard instead). */
static int android_shortcut_for(SDL_Keycode sym)
{
    switch (sym) {
    case SDLK_a: return 29;
    case SDLK_c: return 31;
    case SDLK_x: return 52;
    case SDLK_y: return 53;
    case SDLK_z: return 54;
    default:     return 0;
    }
}

#define ANDROID_META_SHIFT 0x41   /* META_SHIFT_ON | META_SHIFT_LEFT_ON */
#define ANDROID_META_CTRL  0x3000 /* META_CTRL_ON | META_CTRL_LEFT_ON */

static void usage(const char *argv0)
{
    fprintf(stderr,
            "Usage: %s [--host H] [--port P] [--id INSTANCE] [--input-port U]\n"
            "          [--height PX]\n"
            "  --id          go through tebox-api: video on --port (9901),\n"
            "                input as UDP to --input-port (9903); without it\n"
            "                --port is a bare tebox-cast port (view only)\n"
            "  --height  initial window height in points (default 900)\n"
            "Mouse: touch.  Esc / right click back, Home/F1 home, F2 menu,\n"
            "F5 power, PgUp/PgDn volume.  Typing (any IME) goes to the\n"
            "focused field; Cmd+A/C/X/Z/Y act in the guest, Cmd+V pastes the\n"
            "host clipboard.  Clipboards sync both ways.\n",
            argv0);
}

int main(int argc, char **argv)
{
    const char *host = "127.0.0.1";
    const char *id = NULL;
    int port = 9901;
    int input_port = 9903;
    int win_h = 900;
    struct viewer v = { .video_fd = -1, .ctrl_fd = -1 };
    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_Texture *tex = NULL;
    SDL_Thread *thread;
    bool touching = false;
    float touch_x = 0, touch_y = 0;
    uint32_t touch_tick = 0, ping_tick = 0;
    unsigned taps = 0;
    char *last_clip = NULL; /* last text synced either way, to stop echoes */
    uint32_t title_tick = 0;
    uint64_t title_frames = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--host") && i + 1 < argc) {
            host = argv[++i];
        } else if (!strcmp(argv[i], "--port") && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--id") && i + 1 < argc) {
            id = argv[++i];
        } else if (!strcmp(argv[i], "--input-port") && i + 1 < argc) {
            input_port = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--height") && i + 1 < argc) {
            win_h = atoi(argv[++i]);
        } else {
            usage(argv[0]);
            return !strcmp(argv[i], "--help") ? 0 : 1;
        }
    }

    v.host = host;
    v.port = port;
    v.id = id;
    v.video_fd = dial_handshake(host, port, "TEBOX-CAST/1", id);
    if (v.video_fd < 0) {
        fprintf(stderr, "cast-view: cannot connect %s:%d\n", host, port);
        return 1;
    }
    if (id) {
        v.ctrl_fd = open_input_udp(host, input_port);
        if (v.ctrl_fd < 0) {
            fprintf(stderr, "cast-view: input UDP %s:%d unavailable\n", host,
                    input_port);
        }
    }

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "cast-view: SDL: %s\n", SDL_GetError());
        return 1;
    }
    win = SDL_CreateWindow("tebox cast", SDL_WINDOWPOS_CENTERED,
                           SDL_WINDOWPOS_CENTERED, win_h * 9 / 20, win_h,
                           SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED |
                                                SDL_RENDERER_PRESENTVSYNC)
              : NULL;
    if (!ren) {
        fprintf(stderr, "cast-view: SDL window: %s\n", SDL_GetError());
        return 1;
    }

    clipboard_event_type = SDL_RegisterEvents(1);
    if (v.ctrl_fd >= 0) {
        SDL_DetachThread(SDL_CreateThread(control_thread, "cast-control", &v));
        SDL_StartTextInput();
        send_ctrl(&v, "{\"type\":\"ping\"}"); /* subscribe to clipboard */
    }

    v.lock = SDL_CreateMutex();
    v.latest = av_frame_alloc();
    thread = SDL_CreateThread(video_thread, "cast-video", &v);
    fprintf(stderr, "cast-view: %s:%d%s%s input=%s\n", host, port,
            id ? " id=" : "", id ? id : "",
            v.ctrl_fd >= 0 ? "udp" : "off");

    while (!v.quit) {
        SDL_Event ev;
        float gx, gy;

        while (SDL_PollEvent(&ev)) {
            if (getenv("TEBOX_CAST_DEBUG")) {
                if (ev.type == SDL_MOUSEBUTTONDOWN || ev.type == SDL_MOUSEBUTTONUP) {
                    int cx, cy, ww, wh;

                    cursor_in_window(win, &cx, &cy);
                    SDL_GetWindowSize(win, &ww, &wh);
                    fprintf(stderr, "dbg: mouse %s btn=%d event=%d,%d cursor=%d,%d window=%dx%d\n",
                            ev.type == SDL_MOUSEBUTTONDOWN ? "down" : "up",
                            ev.button.button, ev.button.x, ev.button.y,
                            cx, cy, ww, wh);
                } else if (ev.type == SDL_FINGERDOWN || ev.type == SDL_FINGERUP) {
                    fprintf(stderr, "dbg: finger %s x=%.3f y=%.3f\n",
                            ev.type == SDL_FINGERDOWN ? "down" : "up",
                            ev.tfinger.x, ev.tfinger.y);
                } else if (ev.type == SDL_WINDOWEVENT) {
                    fprintf(stderr, "dbg: window event %d\n", ev.window.event);
                } else if (ev.type == SDL_MOUSEMOTION && (SDL_GetTicks() / 500) % 4 == 0) {
                    fprintf(stderr, "dbg: motion x=%d y=%d state=%u\n",
                            ev.motion.x, ev.motion.y, ev.motion.state);
                }
            }
            switch (ev.type) {
            case SDL_QUIT:
                v.quit = true;
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (ev.button.button == SDL_BUTTON_LEFT &&
                    to_guest(&v, win, &gx, &gy)) {
                    touching = true;
                    touch_x = gx;
                    touch_y = gy;
                    touch_tick = SDL_GetTicks();
                    taps++;
                    send_touch(&v, "down", gx, gy);
                } else if (ev.button.button == SDL_BUTTON_RIGHT) {
                    send_key(&v, "back");
                }
                break;
            case SDL_MOUSEMOTION:
                if (touching) {
                    to_guest(&v, win, &gx, &gy);
                    touch_x = gx;
                    touch_y = gy;
                    touch_tick = SDL_GetTicks();
                    send_touch(&v, "move", gx, gy);
                }
                break;
            case SDL_MOUSEBUTTONUP:
                if (ev.button.button == SDL_BUTTON_LEFT && touching) {
                    touching = false;
                    to_guest(&v, win, &gx, &gy);
                    /* Twice: a lost "up" would leave the finger down until
                     * the API's 1 s watchdog releases it. */
                    send_touch(&v, "up", gx, gy);
                    send_touch(&v, "up", gx, gy);
                }
                break;
            case SDL_KEYDOWN: {
                SDL_Keycode sym = ev.key.keysym.sym;
                Uint16 mod = ev.key.keysym.mod;
                const char *nav = nav_key_for(sym);
                int code = android_keycode_for(sym);
                int meta = (mod & KMOD_SHIFT) ? ANDROID_META_SHIFT : 0;

                if (mod & (KMOD_GUI | KMOD_CTRL)) {
                    if (sym == SDLK_v) {
                        char *text = SDL_GetClipboardText();

                        if (text && text[0]) {
                            send_text_event(&v, "clipboard", text,
                                            ",\"paste\":true");
                            free(last_clip);
                            last_clip = strdup(text);
                        }
                        SDL_free(text);
                    } else if ((code = android_shortcut_for(sym))) {
                        send_keycode(&v, code, meta | ANDROID_META_CTRL);
                    }
                } else if (nav) {
                    if (!ev.key.repeat) {
                        send_key(&v, nav);
                    }
                } else if (code) {
                    send_keycode(&v, code, meta);
                }
                break;
            }
            case SDL_TEXTINPUT:
                send_text_event(&v, "text", ev.text.text, "");
                break;
            case SDL_CLIPBOARDUPDATE:
            case SDL_WINDOWEVENT:
                if (ev.type == SDL_WINDOWEVENT &&
                    ev.window.event != SDL_WINDOWEVENT_FOCUS_GAINED) {
                    break;
                }
                if (v.ctrl_fd >= 0 && SDL_HasClipboardText()) {
                    char *text = SDL_GetClipboardText();

                    if (text && text[0] &&
                        (!last_clip || strcmp(text, last_clip))) {
                        send_text_event(&v, "clipboard", text, "");
                        free(last_clip);
                        last_clip = strdup(text);
                    }
                    SDL_free(text);
                }
                break;
            default:
                if (ev.type == clipboard_event_type) {
                    char *text = ev.user.data1;

                    free(last_clip);
                    last_clip = text;
                    SDL_SetClipboardText(text);
                }
                break;
            }
        }

        /* A held finger must keep talking: the API releases a touch that has
         * been silent for 1 s (lost UDP "up"). */
        if (touching && SDL_GetTicks() - touch_tick >= 200) {
            send_touch(&v, "move", touch_x, touch_y);
            touch_tick = SDL_GetTicks();
        }
        if (v.ctrl_fd >= 0 && SDL_GetTicks() - ping_tick >= 5000) {
            send_ctrl(&v, "{\"type\":\"ping\"}"); /* keep clipboard subscription */
            ping_tick = SDL_GetTicks();
        }

        SDL_LockMutex(v.lock);
        if (v.fresh) {
            AVFrame *f = v.latest;

            if (!tex || f->width != v.frame_w || f->height != v.frame_h) {
                if (tex) {
                    SDL_DestroyTexture(tex);
                }
                tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_IYUV,
                                        SDL_TEXTUREACCESS_STREAMING,
                                        f->width, f->height);
                v.frame_w = f->width;
                v.frame_h = f->height;
            }
            if (tex && f->format == AV_PIX_FMT_YUV420P) {
                SDL_UpdateYUVTexture(tex, NULL, f->data[0], f->linesize[0],
                                     f->data[1], f->linesize[1],
                                     f->data[2], f->linesize[2]);
            }
            v.fresh = false;
        }
        SDL_UnlockMutex(v.lock);

        if (SDL_GetTicks() - title_tick >= 1000) {
            char title[128];

            snprintf(title, sizeof(title), "tebox cast %dx%d  %llu fps  taps %u%s",
                     v.frame_w, v.frame_h,
                     (unsigned long long)(v.frames - title_frames), taps,
                     v.ctrl_fd >= 0 ? "" : "  (view only)");
            SDL_SetWindowTitle(win, title);
            title_frames = v.frames;
            title_tick = SDL_GetTicks();
        }

        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        if (tex) {
            int ww, wh, ow, oh;
            SDL_Rect r = fit_rect(win, v.frame_w, v.frame_h);

            /* fit_rect is in window points; scale to drawable pixels. */
            SDL_GetWindowSize(win, &ww, &wh);
            SDL_GetRendererOutputSize(ren, &ow, &oh);
            r.x = r.x * ow / ww;
            r.w = r.w * ow / ww;
            r.y = r.y * oh / wh;
            r.h = r.h * oh / wh;
            SDL_RenderCopy(ren, tex, NULL, &r);
        }
        SDL_RenderPresent(ren);
    }

    if (v.video_fd >= 0) {
        shutdown(v.video_fd, SHUT_RDWR);
    }
    SDL_WaitThread(thread, NULL);
    if (v.video_fd >= 0) {
        close(v.video_fd);
    }
    free(last_clip);
    if (tex) {
        SDL_DestroyTexture(tex);
    }
    av_frame_free(&v.latest);
    SDL_DestroyMutex(v.lock);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
