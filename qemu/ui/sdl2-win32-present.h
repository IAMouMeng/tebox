/* Local Windows presentation workaround; not an upstream QEMU submission.
 * Guest rendering stays on VirGL. Only scanout pixels are read back.
 */
#ifndef SDL2_WIN32_PRESENT_H
#define SDL2_WIN32_PRESENT_H

typedef struct Sdl2Win32Present {
    HWND window;
    uint8_t *pixels;
    BITMAPINFO info;
    int width, height;
    uint32_t last_tick;
} Sdl2Win32Present;

static LRESULT CALLBACK sdl2_win32_present_proc(HWND window, UINT msg,
                                              WPARAM wp, LPARAM lp)
{
    Sdl2Win32Present *p = (void *)GetWindowLongPtrW(window, GWLP_USERDATA);

    if (msg == WM_NCCREATE) {
        p = ((CREATESTRUCTW *)lp)->lpCreateParams;
        SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)p);
    }
    if (msg == WM_PAINT) {
        PAINTSTRUCT paint;
        RECT rect;
        HDC dc = BeginPaint(window, &paint);

        GetClientRect(window, &rect);
        if (p && p->pixels) {
            StretchDIBits(dc, 0, 0, rect.right, rect.bottom, 0, 0,
                          p->width, p->height, p->pixels, &p->info,
                          DIB_RGB_COLORS, SRCCOPY);
        }
        EndPaint(window, &paint);
        return 0;
    }
    if (msg == WM_NCHITTEST) {
        return HTTRANSPARENT;
    }
    return DefWindowProcW(window, msg, wp, lp);
}

void sdl2_gl_win32_present_destroy(struct sdl2_console *scon)
{
    Sdl2Win32Present *p = scon->win32_present;

    if (!p) {
        return;
    }
    DestroyWindow(p->window);
    g_free(p->pixels);
    g_free(p);
    scon->win32_present = NULL;
}

static bool sdl2_win32_present(struct sdl2_console *scon)
{
    Sdl2Win32Present *p = scon->win32_present;
    SDL_SysWMinfo wm;
    RECT rect;
    uint32_t tick = SDL_GetTicks();
    int width = scon->guest_fb.width, height = scon->guest_fb.height;
    GLint read_fb, read_buffer, pack_buffer, alignment, row_length, rows, pixels;
    bool desktop;
    const char *enabled = getenv("TEBOX_WIN32_GDI_PRESENT");

    if (!enabled || strcmp(enabled, "1") || width <= 0 || height <= 0) {
        return false;
    }
    SDL_VERSION(&wm.version);
    if (!SDL_GetWindowWMInfo(scon->real_window, &wm)) {
        return false;
    }
    if (!p) {
        WNDCLASSW wc = {0};

        p = g_new0(Sdl2Win32Present, 1);
        wc.lpfnWndProc = sdl2_win32_present_proc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = L"TeboxGpuReadbackPresent";
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            g_free(p);
            return false;
        }
        /* Disabled child windows leave input with the SDL parent. */
        p->window = CreateWindowExW(0, wc.lpszClassName, L"",
                                    WS_CHILD | WS_VISIBLE | WS_DISABLED,
                                    0, 0, width, height, wm.info.win.window,
                                    NULL, wc.hInstance, p);
        if (!p->window) {
            g_free(p);
            return false;
        }
        scon->win32_present = p;
        fprintf(stderr, "tebox-present: GPU scanout -> Win32 GDI (max 30 fps)\n");
    }
    GetClientRect(wm.info.win.window, &rect);
    MoveWindow(p->window, 0, 0, rect.right, rect.bottom, FALSE);
    if (p->pixels && p->width == width && p->height == height &&
        (uint32_t)(tick - p->last_tick) < 34) {
        return true;
    }
    if (p->width != width || p->height != height) {
        uint8_t *new_pixels = g_try_malloc_n((size_t)width * height, 4);

        if (!new_pixels) {
            sdl2_gl_win32_present_destroy(scon);
            return false;
        }
        g_free(p->pixels);
        p->pixels = new_pixels;
        p->width = width;
        p->height = height;
        memset(&p->info, 0, sizeof(p->info));
        p->info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        p->info.bmiHeader.biWidth = width;
        p->info.bmiHeader.biPlanes = 1;
        p->info.bmiHeader.biBitCount = 32;
        p->info.bmiHeader.biCompression = BI_RGB;
    }
    p->info.bmiHeader.biHeight = scon->y0_top ? height : -height;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fb);
    glGetIntegerv(GL_READ_BUFFER, &read_buffer);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buffer);
    glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &row_length);
    glGetIntegerv(GL_PACK_SKIP_ROWS, &rows);
    glGetIntegerv(GL_PACK_SKIP_PIXELS, &pixels);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, scon->guest_fb.framebuffer);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    desktop = epoxy_is_desktop_gl();
    glReadPixels(0, 0, width, height, desktop ? GL_BGRA : GL_RGBA,
                 GL_UNSIGNED_BYTE, p->pixels);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pack_buffer);
    glPixelStorei(GL_PACK_ALIGNMENT, alignment);
    glPixelStorei(GL_PACK_ROW_LENGTH, row_length);
    glPixelStorei(GL_PACK_SKIP_ROWS, rows);
    glPixelStorei(GL_PACK_SKIP_PIXELS, pixels);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fb);
    glReadBuffer(read_buffer);
    if (!desktop) {
        size_t i;

        for (i = 0; i < (size_t)width * height * 4; i += 4) {
            uint8_t red = p->pixels[i];
            p->pixels[i] = p->pixels[i + 2];
            p->pixels[i + 2] = red;
        }
    }
    p->last_tick = tick;
    InvalidateRect(p->window, NULL, FALSE);
    UpdateWindow(p->window);
    return true;
}
#endif
