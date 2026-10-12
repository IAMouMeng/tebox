package tebox

import (
	"bytes"
	"encoding/json"
	"fmt"
	"net"
	"sync"
	"time"
)

// UDP input: each datagram carries one or more JSON lines (InputEvent). On a
// per-instance control_port the instance is implied; on the shared endpoint
// (ServeInputUDP) every event names its target with "id" or "ids" ("*" = all
// running instances), which makes one datagram drive a whole group.
//
// Replies go back to the sender: {"ok":false,...} for rejected events and
// {"type":"clipboard","id":...,"text":...} for guest clipboard changes. A
// sender receives clipboard updates for udpSubscriberTTL after its last
// datagram; idle clients keep it alive with {"type":"ping"}.

const (
	udpSubscriberTTL = 15 * time.Second
	udpMaxDatagram   = 64 * 1024
)

type udpSubscriber struct {
	cancel  func()
	lastSeq uint64
	seen    time.Time
}

type udpServer struct {
	conn    *net.UDPConn
	resolve func(InputEvent) []*inputInstance
	ids     func(*inputInstance) string

	mu   sync.Mutex
	subs map[string]*udpSubscriber // addr + "|" + instance
}

// ServeInputUDP serves the shared UDP input endpoint until the socket fails.
func (m *Manager) ServeInputUDP(addr string) error {
	udpAddr, err := net.ResolveUDPAddr("udp", addr)
	if err != nil {
		return err
	}
	conn, err := net.ListenUDP("udp", udpAddr)
	if err != nil {
		return err
	}
	go serveUDPWithIDs(conn, m.input.resolve, m.input.idOf)
	return nil
}

func (h *inputHub) idOf(inst *inputInstance) string {
	h.mu.Lock()
	defer h.mu.Unlock()
	for id, v := range h.instances {
		if v == inst {
			return id
		}
	}
	return ""
}

func serveUDP(conn *net.UDPConn, resolve func(InputEvent) []*inputInstance) {
	serveUDPWithIDs(conn, resolve, nil)
}

func serveUDPWithIDs(conn *net.UDPConn, resolve func(InputEvent) []*inputInstance,
	ids func(*inputInstance) string) {
	srv := &udpServer{conn: conn, resolve: resolve, ids: ids, subs: map[string]*udpSubscriber{}}
	go srv.expire()
	buf := make([]byte, udpMaxDatagram)
	for {
		n, addr, err := conn.ReadFromUDP(buf)
		if err != nil {
			srv.closeAll()
			return
		}
		for _, line := range bytes.Split(buf[:n], []byte{'\n'}) {
			if len(bytes.TrimSpace(line)) > 0 {
				srv.handle(addr, line)
			}
		}
	}
}

func (srv *udpServer) reply(addr *net.UDPAddr, v any) {
	if data, err := json.Marshal(v); err == nil {
		_, _ = srv.conn.WriteToUDP(append(data, '\n'), addr)
	}
}

func (srv *udpServer) handle(addr *net.UDPAddr, line []byte) {
	var ev InputEvent
	if err := json.Unmarshal(line, &ev); err != nil {
		srv.reply(addr, map[string]any{"ok": false, "error": "bad json"})
		return
	}
	targets := srv.resolve(ev)
	if len(targets) == 0 {
		srv.reply(addr, map[string]any{"ok": false, "error": ErrInputNotRunning.Error(),
			"id": ev.ID, "seq": ev.Seq})
		return
	}
	for _, inst := range targets {
		if !srv.track(addr, inst, ev.Seq) {
			continue // stale or duplicate datagram
		}
		if ev.Type == "ping" || ev.Type == "subscribe" {
			continue
		}
		if err := inst.send(ev); err != nil {
			srv.reply(addr, map[string]any{"ok": false, "error": err.Error(),
				"id": srv.idFor(inst), "seq": ev.Seq})
		}
	}
}

func (srv *udpServer) idFor(inst *inputInstance) string {
	if srv.ids == nil {
		return ""
	}
	return srv.ids(inst)
}

// track refreshes the sender's clipboard subscription and applies seq order.
func (srv *udpServer) track(addr *net.UDPAddr, inst *inputInstance, seq uint64) bool {
	key := addr.String() + "|" + srvKey(inst)
	srv.mu.Lock()
	sub := srv.subs[key]
	if sub == nil {
		ch, cancel := inst.subscribe()
		sub = &udpSubscriber{cancel: cancel}
		srv.subs[key] = sub
		id := srv.idFor(inst)
		go func() {
			for text := range ch {
				msg := map[string]any{"type": "clipboard", "text": text}
				if id != "" {
					msg["id"] = id
				}
				srv.reply(addr, msg)
			}
		}()
		// The agent pushes guest clipboard changes only once it runs.
		go func() { _, _ = inst.ensureAgent() }()
	}
	sub.seen = time.Now()
	ok := seq == 0 || seq > sub.lastSeq
	if ok && seq != 0 {
		sub.lastSeq = seq
	}
	srv.mu.Unlock()
	return ok
}

// srvKey is per instance object, so a restarted instance resubscribes.
func srvKey(inst *inputInstance) string {
	return fmt.Sprintf("%p", inst)
}

func (srv *udpServer) expire() {
	tick := time.NewTicker(5 * time.Second)
	defer tick.Stop()
	for range tick.C {
		srv.mu.Lock()
		if srv.subs == nil {
			srv.mu.Unlock()
			return
		}
		for key, sub := range srv.subs {
			if time.Since(sub.seen) > udpSubscriberTTL {
				sub.cancel()
				delete(srv.subs, key)
			}
		}
		srv.mu.Unlock()
	}
}

func (srv *udpServer) closeAll() {
	srv.mu.Lock()
	defer srv.mu.Unlock()
	for _, sub := range srv.subs {
		sub.cancel()
	}
	srv.subs = nil
}
