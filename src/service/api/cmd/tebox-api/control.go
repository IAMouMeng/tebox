package main

import (
	"bufio"
	"net"

	tebox "tebox-api/internal/tebox"
)

// Guest input reaches an instance via the shared control endpoint, then
// one QEMU input monitor (see internal/tebox/input.go):
//   - "TEBOX-CTRL/1 <id>\n" on the control endpoint, then JSON lines
//   - UDP datagrams on the same port number (each event names "id")
// No per-instance listen port. Navigation / keycode / text are not on gRPC.

func handleControl(conn net.Conn, reader *bufio.Reader, mgr *tebox.Manager, id string) {
	if tcp, ok := conn.(*net.TCPConn); ok {
		_ = tcp.SetNoDelay(true)
	}
	mgr.ServeInput(id, reader, conn)
}
