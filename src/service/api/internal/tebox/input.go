package tebox

import (
	"bufio"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"regexp"
	"sync"
	"time"
)

// InputEvent is one input event for the guest.
//
//	{"type":"touch","action":"down|move|up","x":0.5,"y":0.25}  x/y in 0..1
//	{"type":"key","key":"home"}                     navigation key, press+release
//	{"type":"text","text":"hello 你好"}              type text (Unicode is pasted)
//	{"type":"keycode","keycode":67,"action":"press","meta":0}
//	                                                Android KeyEvent; down|up|press
//	{"type":"clipboard","text":"...","paste":false} set (and paste) guest clipboard
//
// Touch and key go through QEMU's virtio input devices; text, keycode and
// clipboard go through the guest agent (agent.go), started on first use.
type InputEvent struct {
	// ID / IDs select instances on the shared UDP endpoint ("*" = all
	// running); per-instance ports and gRPC ignore them.
	ID  string   `json:"id,omitempty"`
	IDs []string `json:"ids,omitempty"`
	// Seq, when set, drops reordered UDP datagrams (per sender and instance).
	Seq uint64 `json:"seq,omitempty"`

	Type    string  `json:"type"`
	Action  string  `json:"action,omitempty"`
	X       float64 `json:"x,omitempty"`
	Y       float64 `json:"y,omitempty"`
	Key     string  `json:"key,omitempty"`
	Text    string  `json:"text,omitempty"`
	Keycode int     `json:"keycode,omitempty"`
	Meta    int     `json:"meta,omitempty"`
	Paste   bool    `json:"paste,omitempty"`
}

// Friendly names for Android navigation keys; other QEMU qcodes pass through.
// Home must be KEY_HOMEPAGE (ac_home): Android maps KEY_HOME to MOVE_HOME.
var inputKeys = map[string]string{
	"home": "ac_home", "back": "ac_back", "menu": "menu", "power": "power",
	"volume_up": "volumeup", "volume_down": "volumedown", "mute": "audiomute",
}

var qcodePattern = regexp.MustCompile(`^[a-z0-9_]{1,24}$`)

var ErrInputNotRunning = errors.New("instance is not running")

// inputHub owns, per running instance, the connection to QEMU's input
// monitor (QMP_INPUT_SOCKET) and the plain-TCP control port. QEMU serves one
// client per monitor, so every input source (control port, cast endpoint,
// gRPC) shares that single connection. The zero value is ready to use.
type inputHub struct {
	mu        sync.Mutex
	instances map[string]*inputInstance
}

type inputInstance struct {
	qmpPath     string
	projectRoot string
	adbTarget   string
	listener    net.Listener
	udp         *net.UDPConn // same port number as listener
	port        int
	done        chan struct{}

	mu  sync.Mutex // serialises writes to qmp and guards the touch state
	qmp net.Conn
	// Touch watchdog: UDP may lose an "up"; a held touch must keep sending
	// events (clients send a "move" heartbeat), otherwise it is released.
	touchDown  bool
	touchX     float64
	touchY     float64
	touchStamp time.Time

	agentMu sync.Mutex // guards agent start/replace
	agent   *guestAgent

	subMu sync.Mutex
	subs  map[chan string]struct{} // guest clipboard listeners
	clip  string                   // last clipboard text seen from the guest
}

// start opens the instance's control port. QEMU creates the monitor socket
// itself, so the QMP connection is dialled lazily on the first event.
func (h *inputHub) start(id, qmpPath, projectRoot, adbTarget string) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if h.instances == nil {
		h.instances = make(map[string]*inputInstance)
	}
	if h.instances[id] != nil {
		return
	}
	inst := &inputInstance{qmpPath: qmpPath, projectRoot: projectRoot, adbTarget: adbTarget,
		subs: make(map[chan string]struct{}), done: make(chan struct{})}
	// TCP and UDP share one port number; retry if the UDP half is taken.
	for attempt := 0; attempt < 8 && inst.listener == nil; attempt++ {
		ln, err := net.Listen("tcp", "127.0.0.1:0")
		if err != nil {
			break
		}
		port := ln.Addr().(*net.TCPAddr).Port
		udp, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: port})
		if err != nil {
			ln.Close()
			continue
		}
		inst.listener, inst.udp, inst.port = ln, udp, port
		go inst.accept()
		go serveUDP(udp, func(InputEvent) []*inputInstance { return []*inputInstance{inst} })
	}
	go inst.touchWatchdog()
	h.instances[id] = inst
}

func (h *inputHub) stop(id string) {
	h.mu.Lock()
	inst := h.instances[id]
	delete(h.instances, id)
	h.mu.Unlock()
	if inst == nil {
		return
	}
	close(inst.done)
	if inst.listener != nil {
		_ = inst.listener.Close()
	}
	if inst.udp != nil {
		_ = inst.udp.Close()
	}
	inst.mu.Lock()
	if inst.qmp != nil {
		_ = inst.qmp.Close()
		inst.qmp = nil
	}
	inst.mu.Unlock()
	inst.agentMu.Lock()
	if inst.agent != nil {
		inst.agent.Close()
		inst.agent = nil
	}
	inst.agentMu.Unlock()
	inst.subMu.Lock()
	for ch := range inst.subs {
		close(ch)
		delete(inst.subs, ch)
	}
	inst.subMu.Unlock()
}

func (h *inputHub) stopAll() {
	h.mu.Lock()
	ids := make([]string, 0, len(h.instances))
	for id := range h.instances {
		ids = append(ids, id)
	}
	h.mu.Unlock()
	for _, id := range ids {
		h.stop(id)
	}
}

// resolve maps an event's id/ids ("*" = every running instance).
func (h *inputHub) resolve(ev InputEvent) []*inputInstance {
	h.mu.Lock()
	defer h.mu.Unlock()
	ids := ev.IDs
	if ev.ID != "" {
		ids = append([]string{ev.ID}, ids...)
	}
	var out []*inputInstance
	seen := map[*inputInstance]bool{}
	for _, id := range ids {
		if id == "*" {
			for _, inst := range h.instances {
				if !seen[inst] {
					seen[inst] = true
					out = append(out, inst)
				}
			}
		} else if inst := h.instances[id]; inst != nil && !seen[inst] {
			seen[inst] = true
			out = append(out, inst)
		}
	}
	return out
}

func (h *inputHub) get(id string) *inputInstance {
	h.mu.Lock()
	defer h.mu.Unlock()
	return h.instances[id]
}

func (h *inputHub) port(id string) int {
	if inst := h.get(id); inst != nil {
		return inst.port
	}
	return 0
}

// ControlPort is the per-instance plain-TCP input port (0 when not running).
func (m *Manager) ControlPort(id string) int { return m.input.port(id) }

// SendInput injects one touch or key event into a running instance.
func (m *Manager) SendInput(id string, ev InputEvent) error {
	inst := m.input.get(id)
	if inst == nil {
		return ErrInputNotRunning
	}
	return inst.send(ev)
}

// WatchClipboard streams guest clipboard changes until cancel is called or
// the instance stops (the channel is then closed). It starts the guest agent.
func (m *Manager) WatchClipboard(id string) (<-chan string, func(), error) {
	inst := m.input.get(id)
	if inst == nil {
		return nil, nil, ErrInputNotRunning
	}
	if _, err := inst.ensureAgent(); err != nil {
		return nil, nil, err
	}
	ch, cancel := inst.subscribe()
	return ch, cancel, nil
}

// ServeInput reads JSON lines from r until EOF and injects each event.
// It writes one {"ok":true} line first, an error line per rejected event,
// and {"type":"clipboard","text":...} whenever the guest clipboard changes.
func (m *Manager) ServeInput(id string, r *bufio.Reader, w io.Writer) {
	inst := m.input.get(id)
	if inst == nil {
		fmt.Fprintf(w, "{\"ok\":false,\"error\":%q}\n", ErrInputNotRunning.Error())
		return
	}
	inst.serve(r, w)
}

func (inst *inputInstance) accept() {
	for {
		conn, err := inst.listener.Accept()
		if err != nil {
			return
		}
		go func() {
			defer conn.Close()
			if tcp, ok := conn.(*net.TCPConn); ok {
				_ = tcp.SetNoDelay(true)
			}
			inst.serve(bufio.NewReader(conn), conn)
		}()
	}
}

func (inst *inputInstance) serve(r *bufio.Reader, w io.Writer) {
	var wmu sync.Mutex
	writeLine := func(v any) error {
		line, err := json.Marshal(v)
		if err != nil {
			return err
		}
		wmu.Lock()
		defer wmu.Unlock()
		_, err = w.Write(append(line, '\n'))
		return err
	}
	if writeLine(map[string]any{"ok": true}) != nil {
		return
	}
	clips, cancel := inst.subscribe()
	defer cancel()
	go func() {
		for text := range clips {
			_ = writeLine(map[string]any{"type": "clipboard", "text": text})
		}
	}()
	// Start the agent early so guest copies reach this client; failures
	// (guest still booting) are retried by the next agent event.
	go func() { _, _ = inst.ensureAgent() }()
	for {
		line, err := r.ReadBytes('\n')
		if len(line) > 1 {
			var ev InputEvent
			sendErr := json.Unmarshal(line, &ev)
			if sendErr == nil {
				sendErr = inst.send(ev)
			}
			if sendErr != nil {
				if writeLine(map[string]any{"ok": false, "error": sendErr.Error()}) != nil {
					return
				}
			}
		}
		if err != nil {
			return
		}
	}
}

func (inst *inputInstance) subscribe() (chan string, func()) {
	ch := make(chan string, 8)
	inst.subMu.Lock()
	inst.subs[ch] = struct{}{}
	inst.subMu.Unlock()
	var once sync.Once
	return ch, func() {
		once.Do(func() {
			inst.subMu.Lock()
			if _, ok := inst.subs[ch]; ok {
				delete(inst.subs, ch)
				close(ch)
			}
			inst.subMu.Unlock()
		})
	}
}

func (inst *inputInstance) publishClipboard(text string) {
	inst.subMu.Lock()
	defer inst.subMu.Unlock()
	if text == inst.clip {
		return
	}
	inst.clip = text
	for ch := range inst.subs {
		select {
		case ch <- text:
		default: // slow listener: drop rather than stall the agent
		}
	}
}

func (inst *inputInstance) ensureAgent() (*guestAgent, error) {
	inst.agentMu.Lock()
	defer inst.agentMu.Unlock()
	if inst.agent != nil && inst.agent.alive() {
		return inst.agent, nil
	}
	a, err := startAgent(inst.projectRoot, inst.adbTarget, inst.publishClipboard)
	if err != nil {
		return nil, fmt.Errorf("guest agent: %w", err)
	}
	inst.agent = a
	return a, nil
}

func (inst *inputInstance) sendAgent(ev InputEvent) error {
	a, err := inst.ensureAgent()
	if err != nil {
		return err
	}
	switch ev.Type {
	case "text":
		if !isInjectableText(ev.Text) {
			// Pasted through the clipboard: do not echo it to host clipboards.
			inst.subMu.Lock()
			inst.clip = ev.Text
			inst.subMu.Unlock()
		}
		err = a.Text(ev.Text)
	case "clipboard":
		inst.subMu.Lock()
		inst.clip = ev.Text // the guest echoes it back; do not bounce it
		inst.subMu.Unlock()
		err = a.SetClipboard(ev.Text, ev.Paste)
	case "keycode":
		if ev.Keycode <= 0 {
			return fmt.Errorf("invalid keycode %d", ev.Keycode)
		}
		switch ev.Action {
		case "down":
			err = a.Keycode(0, ev.Keycode, ev.Meta)
		case "up":
			err = a.Keycode(1, ev.Keycode, ev.Meta)
		case "", "press":
			if err = a.Keycode(0, ev.Keycode, ev.Meta); err == nil {
				err = a.Keycode(1, ev.Keycode, ev.Meta)
			}
		default:
			return fmt.Errorf("unknown keycode action %q", ev.Action)
		}
	}
	return err
}

func (inst *inputInstance) send(ev InputEvent) error {
	switch ev.Type {
	case "text", "keycode", "clipboard":
		return inst.sendAgent(ev)
	}
	cmd, err := qmpInputCommand(ev)
	if err != nil {
		return err
	}
	inst.mu.Lock()
	defer inst.mu.Unlock()
	if ev.Type == "touch" {
		inst.touchDown = ev.Action != "up"
		inst.touchX, inst.touchY, inst.touchStamp = ev.X, ev.Y, time.Now()
	}
	return inst.writeQMPLocked(cmd)
}

const touchHoldTimeout = time.Second

func (inst *inputInstance) touchWatchdog() {
	tick := time.NewTicker(250 * time.Millisecond)
	defer tick.Stop()
	for {
		select {
		case <-inst.done:
			return
		case <-tick.C:
		}
		inst.mu.Lock()
		if inst.touchDown && time.Since(inst.touchStamp) > touchHoldTimeout {
			inst.touchDown = false
			if cmd, err := qmpInputCommand(InputEvent{Type: "touch", Action: "up",
				X: inst.touchX, Y: inst.touchY}); err == nil {
				_ = inst.writeQMPLocked(cmd)
			}
		}
		inst.mu.Unlock()
	}
}

func (inst *inputInstance) writeQMPLocked(cmd []byte) (err error) {
	// One retry: the monitor connection drops if QEMU restarts the socket.
	for attempt := 0; attempt < 2; attempt++ {
		if inst.qmp == nil {
			if inst.qmp, err = openInputQMP(inst.qmpPath); err != nil {
				return fmt.Errorf("input monitor: %w", err)
			}
		}
		if _, err = inst.qmp.Write(cmd); err == nil {
			return nil
		}
		_ = inst.qmp.Close()
		inst.qmp = nil
	}
	return fmt.Errorf("input monitor: %w", err)
}

// openInputQMP negotiates capabilities, then discards replies in the
// background so QEMU never blocks on a full socket buffer.
func openInputQMP(path string) (net.Conn, error) {
	conn, err := net.DialTimeout("unix", path, time.Second)
	if err != nil {
		return nil, err
	}
	r := bufio.NewReader(conn)
	_ = conn.SetDeadline(time.Now().Add(2 * time.Second))
	if _, err = r.ReadBytes('\n'); err == nil { // greeting
		if _, err = io.WriteString(conn, "{\"execute\":\"qmp_capabilities\"}\n"); err == nil {
			_, err = r.ReadBytes('\n')
		}
	}
	if err != nil {
		conn.Close()
		return nil, err
	}
	_ = conn.SetDeadline(time.Time{})
	go func() { _, _ = io.Copy(io.Discard, r) }()
	return conn, nil
}

const absMax = 0x7fff // QEMU INPUT_EVENT_ABS_MAX

func qmpInputCommand(ev InputEvent) ([]byte, error) {
	type data map[string]any
	type event struct {
		Type string `json:"type"`
		Data data   `json:"data"`
	}
	var events []event
	switch ev.Type {
	case "touch":
		abs := func(v float64) int {
			if v < 0 {
				v = 0
			} else if v > 1 {
				v = 1
			}
			return int(v*absMax + 0.5)
		}
		events = []event{
			{"abs", data{"axis": "x", "value": abs(ev.X)}},
			{"abs", data{"axis": "y", "value": abs(ev.Y)}},
		}
		switch ev.Action {
		case "down":
			events = append(events, event{"btn", data{"button": "left", "down": true}})
		case "up":
			events = append(events, event{"btn", data{"button": "left", "down": false}})
		case "move":
		default:
			return nil, fmt.Errorf("unknown touch action %q", ev.Action)
		}
	case "key":
		code := inputKeys[ev.Key]
		if code == "" && qcodePattern.MatchString(ev.Key) {
			code = ev.Key
		}
		if code == "" {
			return nil, fmt.Errorf("unknown key %q", ev.Key)
		}
		key := data{"type": "qcode", "data": code}
		events = []event{
			{"key", data{"key": key, "down": true}},
			{"key", data{"key": key, "down": false}},
		}
	default:
		return nil, fmt.Errorf("unknown event type %q", ev.Type)
	}
	out, err := json.Marshal(map[string]any{
		"execute":   "input-send-event",
		"arguments": map[string]any{"events": events},
	})
	if err != nil {
		return nil, err
	}
	return append(out, '\n'), nil
}
