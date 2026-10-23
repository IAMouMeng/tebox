package main

import (
	"bufio"
	"net"

	tebox "tebox-api/internal/tebox"
)

// Guest input reaches an instance on TCP/UDP, sharing one QEMU input
// monitor (see internal/tebox/input.go):
//   - Instance.control_port: plain TCP, JSON lines (tebox.InputEvent)
//   - "TEBOX-CTRL/1 <id>\n" on the control endpoint (9905), then JSON lines
//   - UDP datagrams on the same port number
// Navigation keys, Android keycodes, text and clipboard are not on gRPC.

func handleControl(conn net.Conn, reader *bufio.Reader, mgr *tebox.Manager, id string) {
	if tcp, ok := conn.(*net.TCPConn); ok {
		_ = tcp.SetNoDelay(true)
	}
	mgr.ServeInput(id, reader, conn)
}
