package main

import (
	"bufio"
	"encoding/binary"
	"fmt"
	"io"
	"net"
	"strings"
	"time"

	tebox "tebox-api/internal/tebox"
)

// Each instance's tebox-cast serves one interleaved stream (video, audio,
// camera/mic uplink; see cast/include/cast/protocol.h). The API splits it
// over three LAN endpoints, every connection naming its instance first:
//
//	cast    (9901) "TEBOX-CAST/1 <id>\n"    video records out; nothing is read back
//	audio   (9904) "TEBOX-AUDIO/1 <id>\n"   AAC records out; microphone PCM records in
//	control (9905) "TEBOX-CTRL/1 <id>\n"    input JSON lines (both ways)
//	               "TEBOX-CAMERA/1 <id>\n"  camera JPEG records in
//	               UDP on the same number: input datagrams (udp.go)

const (
	codecAAC   = 16
	codecMJPEG = 32
	codecPCM   = 33

	recordHeader = 10      // u8 codec, u8 flags, u64 pts after the u32 length
	uplinkMax    = 4 << 20 // CAST_UPLINK_MAX
	downlinkMax  = 32 << 20
)

func serve(listener net.Listener, handle func(net.Conn)) {
	for {
		conn, err := listener.Accept()
		if err != nil {
			return
		}
		go func() {
			defer conn.Close()
			handle(conn)
		}()
	}
}

// handshake reads "<proto> <id>\n" within 5 s.
func handshake(conn net.Conn) (*bufio.Reader, string, string, bool) {
	_ = conn.SetReadDeadline(time.Now().Add(5 * time.Second))
	reader := bufio.NewReader(conn)
	line, err := reader.ReadString('\n')
	if err != nil {
		return nil, "", "", false
	}
	_ = conn.SetReadDeadline(time.Time{})
	parts := strings.Fields(strings.TrimSpace(line))
	if len(parts) != 2 {
		return nil, "", "", false
	}
	return reader, parts[0], parts[1], true
}

func handleCast(conn net.Conn, mgr *tebox.Manager) {
	_, proto, id, ok := handshake(conn)
	if !ok || proto != "TEBOX-CAST/1" {
		return
	}
	backend, err := dialCast(mgr, id)
	if err != nil {
		return
	}
	defer backend.Close()
	// Video only, and the viewer sends nothing: anything it writes is
	// discarded so it can never reach the instance.
	done := make(chan struct{}, 2)
	go func() { relayRecords(conn, backend, downlinkMax, isVideo); done <- struct{}{} }()
	go func() { _, _ = io.Copy(io.Discard, conn); done <- struct{}{} }()
	<-done
}

func handleAudio(conn net.Conn, mgr *tebox.Manager) {
	reader, proto, id, ok := handshake(conn)
	if !ok || proto != "TEBOX-AUDIO/1" {
		return
	}
	backend, err := dialCast(mgr, id)
	if err != nil {
		return
	}
	defer backend.Close()
	done := make(chan struct{}, 2)
	go func() { relayRecords(conn, backend, downlinkMax, codecIs(codecAAC)); done <- struct{}{} }()
	go func() { relayRecords(backend, reader, uplinkMax, codecIs(codecPCM)); done <- struct{}{} }()
	<-done
}

func handleControlConn(conn net.Conn, mgr *tebox.Manager) {
	reader, proto, id, ok := handshake(conn)
	if !ok {
		return
	}
	switch proto {
	case "TEBOX-CTRL/1":
		handleControl(conn, reader, mgr, id)
	case "TEBOX-CAMERA/1":
		backend, err := dialCast(mgr, id)
		if err != nil {
			return
		}
		defer backend.Close()
		// tebox-cast streams video to every connection; drain it here.
		go func() { _, _ = io.Copy(io.Discard, backend) }()
		relayRecords(backend, reader, uplinkMax, codecIs(codecMJPEG))
	}
}

func dialCast(mgr *tebox.Manager, id string) (net.Conn, error) {
	port, err := mgr.CastPort(id)
	if err != nil {
		return nil, err
	}
	return net.DialTimeout("tcp", fmt.Sprintf("127.0.0.1:%d", port), 2*time.Second)
}

func isVideo(codec byte) bool { return codec < codecAAC }

func codecIs(want byte) func(byte) bool { return func(c byte) bool { return c == want } }

// relayRecords copies whole cast records from src to dst, keeping only the
// codecs keep accepts. It stops on EOF, a write error or a malformed record.
func relayRecords(dst io.Writer, src io.Reader, max uint32, keep func(byte) bool) {
	var lenbuf [4]byte
	buf := make([]byte, 0, 64<<10)
	for {
		if _, err := io.ReadFull(src, lenbuf[:]); err != nil {
			return
		}
		n := binary.BigEndian.Uint32(lenbuf[:])
		if n < recordHeader || n > max {
			return
		}
		if cap(buf) < int(4+n) {
			buf = make([]byte, 0, 4+n)
		}
		buf = append(buf[:0], lenbuf[:]...)
		buf = buf[:4+n]
		if _, err := io.ReadFull(src, buf[4:]); err != nil {
			return
		}
		if !keep(buf[4]) {
			continue
		}
		if _, err := dst.Write(buf); err != nil {
			return
		}
	}
}
