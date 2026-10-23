package tebox

import (
	"bytes"
	"context"
	"fmt"
	"image"
	"image/color"
	"image/jpeg"
	"image/png"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"time"
)

type PreviewOptions struct {
	Size    int    // longest side in pixels; 0 keeps native
	Format  string // jpeg (default) or png
	Quality int    // 1..100, jpeg only; default 80
}

type Preview struct {
	Data   []byte
	Format string
	Mime   string
	Width  int
	Height int
}

func (m *Manager) Preview(id string, opts PreviewOptions) (Preview, error) {
	item, err := m.Get(id)
	if err != nil {
		return Preview{}, err
	}
	if !item.Running {
		return Preview{}, ErrInputNotRunning
	}
	format, mime, err := normalizePreviewFormat(opts.Format)
	if err != nil {
		return Preview{}, err
	}
	quality := opts.Quality
	if quality <= 0 {
		quality = 80
	}
	if quality > 100 {
		quality = 100
	}

	img, err := m.captureScreen(item)
	if err != nil {
		return Preview{}, err
	}
	img = scalePreview(img, opts.Size)

	var buf bytes.Buffer
	switch format {
	case "png":
		if err := png.Encode(&buf, img); err != nil {
			return Preview{}, err
		}
	default:
		if err := jpeg.Encode(&buf, img, &jpeg.Options{Quality: quality}); err != nil {
			return Preview{}, err
		}
	}
	b := img.Bounds()
	return Preview{
		Data:   buf.Bytes(),
		Format: format,
		Mime:   mime,
		Width:  b.Dx(),
		Height: b.Dy(),
	}, nil
}

func normalizePreviewFormat(raw string) (format, mime string, err error) {
	switch strings.ToLower(strings.TrimSpace(raw)) {
	case "", "jpeg", "jpg":
		return "jpeg", "image/jpeg", nil
	case "png":
		return "png", "image/png", nil
	default:
		return "", "", fmt.Errorf("%w: format must be jpeg or png", ErrInvalidOptions)
	}
}

func (m *Manager) captureScreen(item Instance) (image.Image, error) {
	if img, err := captureAdb(item.AdbPort); err == nil {
		return img, nil
	}
	qmp := filepath.Join(item.Dir, "qmp.sock")
	dump := filepath.Join(item.Dir, "preview-dump.ppm")
	_ = os.Remove(dump)
	if err := Screendump(qmp, dump); err != nil {
		return nil, fmt.Errorf("preview: adb screencap and qmp screendump failed: %w", err)
	}
	defer os.Remove(dump)
	data, err := os.ReadFile(dump)
	if err != nil {
		return nil, err
	}
	return decodePPM(data)
}

func captureAdb(port int) (image.Image, error) {
	if port <= 0 {
		return nil, fmt.Errorf("no adb port")
	}
	target := fmt.Sprintf("127.0.0.1:%d", port)
	adb := adbBinary()
	ctx, cancel := context.WithTimeout(context.Background(), 8*time.Second)
	defer cancel()
	_ = exec.CommandContext(ctx, adb, "connect", target).Run()
	out, err := exec.CommandContext(ctx, adb, "-s", target, "exec-out", "screencap", "-p").Output()
	if err != nil {
		return nil, err
	}
	img, _, err := image.Decode(bytes.NewReader(out))
	return img, err
}

func scalePreview(src image.Image, maxSize int) image.Image {
	b := src.Bounds()
	srcW, srcH := b.Dx(), b.Dy()
	if srcW <= 0 || srcH <= 0 {
		return src
	}
	dstW, dstH := fitPreviewSize(srcW, srcH, maxSize)
	if dstW == srcW && dstH == srcH {
		return src
	}
	dst := image.NewRGBA(image.Rect(0, 0, dstW, dstH))
	for y := 0; y < dstH; y++ {
		sy := b.Min.Y + y*srcH/dstH
		for x := 0; x < dstW; x++ {
			sx := b.Min.X + x*srcW/dstW
			dst.Set(x, y, src.At(sx, sy))
		}
	}
	return dst
}

func fitPreviewSize(srcW, srcH, maxSize int) (int, int) {
	const capPx = 4096
	if maxSize <= 0 {
		return srcW, srcH
	}
	if maxSize > capPx {
		maxSize = capPx
	}
	long := srcW
	if srcH > long {
		long = srcH
	}
	if long == maxSize {
		return srcW, srcH
	}
	dstW := srcW * maxSize / long
	dstH := srcH * maxSize / long
	if dstW < 1 {
		dstW = 1
	}
	if dstH < 1 {
		dstH = 1
	}
	return dstW, dstH
}

func decodePPM(data []byte) (image.Image, error) {
	if len(data) < 7 || !bytes.HasPrefix(data, []byte("P6")) {
		return nil, fmt.Errorf("screendump is not a P6 PPM")
	}
	rest := data[2:]
	var (
		fields []int
		i      int
	)
	for i < len(rest) && len(fields) < 3 {
		for i < len(rest) && (rest[i] == ' ' || rest[i] == '\t' || rest[i] == '\n' || rest[i] == '\r') {
			i++
		}
		if i < len(rest) && rest[i] == '#' {
			for i < len(rest) && rest[i] != '\n' {
				i++
			}
			continue
		}
		start := i
		for i < len(rest) && rest[i] >= '0' && rest[i] <= '9' {
			i++
		}
		if start == i {
			return nil, fmt.Errorf("invalid PPM header")
		}
		n, err := strconv.Atoi(string(rest[start:i]))
		if err != nil {
			return nil, fmt.Errorf("invalid PPM header: %w", err)
		}
		fields = append(fields, n)
	}
	if len(fields) != 3 || fields[0] <= 0 || fields[1] <= 0 || fields[2] != 255 {
		return nil, fmt.Errorf("unsupported PPM header")
	}
	if i < len(rest) && (rest[i] == '\n' || rest[i] == '\r' || rest[i] == ' ') {
		i++
	}
	w, h := fields[0], fields[1]
	pixels := rest[i:]
	need := w * h * 3
	if len(pixels) < need {
		return nil, fmt.Errorf("truncated PPM (%d < %d)", len(pixels), need)
	}
	img := image.NewRGBA(image.Rect(0, 0, w, h))
	for y := 0; y < h; y++ {
		for x := 0; x < w; x++ {
			off := (y*w + x) * 3
			img.SetRGBA(x, y, color.RGBA{
				R: pixels[off], G: pixels[off+1], B: pixels[off+2], A: 255,
			})
		}
	}
	return img, nil
}
