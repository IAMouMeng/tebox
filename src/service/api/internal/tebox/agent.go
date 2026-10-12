package tebox

import (
	"bufio"
	"bytes"
	"crypto/rand"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"
	"unicode/utf8"
)

// The guest agent is an unmodified scrcpy-server (prebuilts/android-arm64/
// scrcpy) started over adb with video and audio disabled. Only its control
// socket is used: text injection, Android keycodes and clipboard sync, which
// QEMU's virtio keyboard cannot express (no Unicode, no clipboard).

const (
	agentRemoteJar = "/data/local/tmp/tebox-scrcpy.jar"

	scrcpyInjectKeycode = 0
	scrcpyInjectText    = 1
	scrcpySetClipboard  = 9
	scrcpyMsgClipboard  = 0
	scrcpyMsgAckClip    = 1
	scrcpyMsgUhidOutput = 2

	scrcpyTextMax = 300 // INJECT_TEXT_MAX_LENGTH
)

type guestAgent struct {
	conn    net.Conn
	server  *exec.Cmd
	adb     string
	target  string
	forward string

	writeMu sync.Mutex
	seq     uint64
	closed  chan struct{}
}

func adbBinary() string {
	if adb := os.Getenv("TEBOX_ADB"); adb != "" {
		return adb
	}
	return "adb"
}

func agentServer(projectRoot string) (path, version string, err error) {
	path = os.Getenv("TEBOX_SCRCPY_SERVER")
	dir := filepath.Join(projectRoot, "prebuilts", "android-arm64", "scrcpy")
	if path == "" {
		path = filepath.Join(dir, "scrcpy-server")
	}
	version = os.Getenv("TEBOX_SCRCPY_VERSION")
	if version == "" {
		raw, rerr := os.ReadFile(filepath.Join(dir, "VERSION"))
		if rerr != nil {
			return "", "", fmt.Errorf("scrcpy-server version: %w", rerr)
		}
		version = strings.TrimSpace(string(raw))
	}
	if _, err = os.Stat(path); err != nil {
		return "", "", fmt.Errorf("scrcpy-server: %w", err)
	}
	return path, version, nil
}

// startAgent pushes and launches the agent in the guest, then connects to
// its control socket. onClipboard receives every guest clipboard change.
func startAgent(projectRoot, target string, onClipboard func(string)) (*guestAgent, error) {
	jar, version, err := agentServer(projectRoot)
	if err != nil {
		return nil, err
	}
	a := &guestAgent{adb: adbBinary(), target: target, closed: make(chan struct{})}
	_ = exec.Command(a.adb, "connect", target).Run()
	if out, err := a.run("push", jar, agentRemoteJar); err != nil {
		return nil, fmt.Errorf("adb push: %w (%s)", err, out)
	}
	var id [4]byte
	_, _ = rand.Read(id[:])
	id[0] &= 0x7f // scid is a positive 31-bit value
	scid := hex.EncodeToString(id[:])
	out, err := a.run("forward", "tcp:0", "localabstract:scrcpy_"+scid)
	if err != nil {
		return nil, fmt.Errorf("adb forward: %w (%s)", err, out)
	}
	port, err := strconv.Atoi(strings.TrimSpace(out))
	if err != nil {
		return nil, fmt.Errorf("adb forward port %q", out)
	}
	a.forward = "tcp:" + strconv.Itoa(port)

	a.server = exec.Command(a.adb, "-s", target, "shell",
		"CLASSPATH="+agentRemoteJar, "app_process", "/", "com.genymobile.scrcpy.Server", version,
		"scid="+scid, "log_level=warn", "video=false", "audio=false", "control=true",
		"tunnel_forward=true", "send_device_meta=false", "send_dummy_byte=true",
		"clipboard_autosync=true", "cleanup=false", "power_on=false")
	a.server.Stdout, a.server.Stderr = os.Stderr, os.Stderr
	if err := a.server.Start(); err != nil {
		a.removeForward()
		return nil, err
	}
	go func() { _ = a.server.Wait() }()

	// adb accepts the forwarded connection before the server listens; the
	// dummy byte confirms the server end is really there.
	deadline := time.Now().Add(10 * time.Second)
	for {
		conn, derr := net.DialTimeout("tcp", fmt.Sprintf("127.0.0.1:%d", port), time.Second)
		if derr == nil {
			_ = conn.SetReadDeadline(time.Now().Add(time.Second))
			var b [1]byte
			if _, derr = io.ReadFull(conn, b[:]); derr == nil {
				_ = conn.SetReadDeadline(time.Time{})
				a.conn = conn
				break
			}
			conn.Close()
		}
		if time.Now().After(deadline) {
			a.Close()
			return nil, fmt.Errorf("guest agent did not start: %w", derr)
		}
		time.Sleep(150 * time.Millisecond)
	}
	go a.readLoop(onClipboard)
	return a, nil
}

func (a *guestAgent) run(args ...string) (string, error) {
	out, err := exec.Command(a.adb, append([]string{"-s", a.target}, args...)...).CombinedOutput()
	return strings.TrimSpace(string(out)), err
}

func (a *guestAgent) removeForward() {
	if a.forward != "" {
		_, _ = a.run("forward", "--remove", a.forward)
	}
}

func (a *guestAgent) Close() {
	select {
	case <-a.closed:
		return
	default:
		close(a.closed)
	}
	if a.conn != nil {
		_ = a.conn.Close()
	}
	if a.server != nil && a.server.Process != nil {
		_ = a.server.Process.Kill()
	}
	a.removeForward()
}

func (a *guestAgent) alive() bool {
	select {
	case <-a.closed:
		return false
	default:
		return true
	}
}

func (a *guestAgent) readLoop(onClipboard func(string)) {
	defer a.Close()
	r := bufio.NewReader(a.conn)
	for {
		typ, err := r.ReadByte()
		if err != nil {
			return
		}
		switch typ {
		case scrcpyMsgClipboard:
			var n uint32
			if binary.Read(r, binary.BigEndian, &n) != nil || n > 1<<18 {
				return
			}
			text := make([]byte, n)
			if _, err := io.ReadFull(r, text); err != nil {
				return
			}
			if onClipboard != nil {
				onClipboard(string(text))
			}
		case scrcpyMsgAckClip:
			if _, err := io.ReadFull(r, make([]byte, 8)); err != nil {
				return
			}
		case scrcpyMsgUhidOutput:
			var hdr [4]byte
			if _, err := io.ReadFull(r, hdr[:]); err != nil {
				return
			}
			if _, err := io.ReadFull(r, make([]byte, binary.BigEndian.Uint16(hdr[2:]))); err != nil {
				return
			}
		default:
			return
		}
	}
}

func (a *guestAgent) write(msg []byte) error {
	a.writeMu.Lock()
	defer a.writeMu.Unlock()
	_ = a.conn.SetWriteDeadline(time.Now().Add(2 * time.Second))
	_, err := a.conn.Write(msg)
	if err != nil {
		go a.Close()
	}
	return err
}

func isInjectableText(s string) bool {
	for _, r := range s {
		if r < 0x20 && r != '\n' && r != '\t' || r > 0x7e {
			return false
		}
	}
	return true
}

// Text types ASCII through key events and pastes everything else (CJK,
// emoji) through the clipboard, as Android key maps cannot type it.
func (a *guestAgent) Text(text string) error {
	if text == "" {
		return nil
	}
	if !isInjectableText(text) {
		return a.SetClipboard(text, true)
	}
	for len(text) > 0 {
		n := len(text)
		if n > scrcpyTextMax {
			n = scrcpyTextMax
		}
		var b bytes.Buffer
		b.WriteByte(scrcpyInjectText)
		_ = binary.Write(&b, binary.BigEndian, uint32(n))
		b.WriteString(text[:n])
		if err := a.write(b.Bytes()); err != nil {
			return err
		}
		text = text[n:]
	}
	return nil
}

// Keycode injects an Android KeyEvent (action 0 = down, 1 = up).
func (a *guestAgent) Keycode(action, keycode, meta int) error {
	var b bytes.Buffer
	b.WriteByte(scrcpyInjectKeycode)
	b.WriteByte(byte(action))
	_ = binary.Write(&b, binary.BigEndian, int32(keycode))
	_ = binary.Write(&b, binary.BigEndian, int32(0)) // repeat
	_ = binary.Write(&b, binary.BigEndian, int32(meta))
	return a.write(b.Bytes())
}

// SetClipboard sets the guest clipboard and optionally pastes it.
func (a *guestAgent) SetClipboard(text string, paste bool) error {
	if !utf8.ValidString(text) {
		return errors.New("clipboard text is not UTF-8")
	}
	if len(text) > 1<<18-14 {
		return errors.New("clipboard text too long")
	}
	a.writeMu.Lock()
	a.seq++
	seq := a.seq
	a.writeMu.Unlock()
	var b bytes.Buffer
	b.WriteByte(scrcpySetClipboard)
	_ = binary.Write(&b, binary.BigEndian, seq)
	if paste {
		b.WriteByte(1)
	} else {
		b.WriteByte(0)
	}
	_ = binary.Write(&b, binary.BigEndian, uint32(len(text)))
	b.WriteString(text)
	return a.write(b.Bytes())
}
