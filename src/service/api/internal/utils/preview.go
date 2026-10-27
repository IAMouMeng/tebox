package utils

import (
	"bytes"
	"fmt"
	"image"
	"image/color"
	"strconv"
	"strings"

	"tebox-api/internal/constants"
)

// NormalizePreviewFormat returns wire format name and MIME type.
func NormalizePreviewFormat(raw string) (format, mime string, err error) {
	switch strings.ToLower(strings.TrimSpace(raw)) {
	case "", constants.PreviewFormatJPEG, "jpg":
		return constants.PreviewFormatJPEG, "image/jpeg", nil
	case constants.PreviewFormatPNG:
		return constants.PreviewFormatPNG, "image/png", nil
	default:
		return "", "", fmt.Errorf("format must be jpeg or png")
	}
}

// ScalePreview downscales so the longest side is maxSize (0 = identity).
func ScalePreview(src image.Image, maxSize int) image.Image {
	b := src.Bounds()
	srcW, srcH := b.Dx(), b.Dy()
	if srcW <= 0 || srcH <= 0 {
		return src
	}
	dstW, dstH := FitPreviewSize(srcW, srcH, maxSize)
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

// FitPreviewSize computes destination dimensions for a longest-side cap.
func FitPreviewSize(srcW, srcH, maxSize int) (int, int) {
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

// DecodePPM decodes a P6 PPM framebuffer dump.
func DecodePPM(data []byte) (image.Image, error) {
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
