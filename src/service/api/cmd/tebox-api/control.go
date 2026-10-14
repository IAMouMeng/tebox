package main

import (
	"bufio"
	"context"
	"io"
	"net"

	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
	api "tebox-api/api/v1"
	tebox "tebox-api/internal/tebox"
)

// Guest input reaches an instance three ways, all sharing one QEMU input
// monitor (see internal/tebox/input.go):
//   - Instance.control_port: plain TCP, JSON lines (tebox.InputEvent)
//   - "TEBOX-CTRL/1 <id>\n" on the control endpoint (9905), then JSON lines
//   - gRPC SendInput / StreamInput (and WatchClipboard for guest copies)

func handleControl(conn net.Conn, reader *bufio.Reader, mgr *tebox.Manager, id string) {
	if tcp, ok := conn.(*net.TCPConn); ok {
		_ = tcp.SetNoDelay(true)
	}
	mgr.ServeInput(id, reader, conn)
}

func (s *server) SendInput(_ context.Context, req *api.InputRequest) (*api.Empty, error) {
	if err := s.sendInput(req); err != nil {
		return nil, err
	}
	return &api.Empty{}, nil
}

func (s *server) StreamInput(stream api.AndroidService_StreamInputServer) error {
	for {
		req, err := stream.Recv()
		if err == io.EOF {
			return stream.SendAndClose(&api.Empty{})
		}
		if err != nil {
			return err
		}
		if err := s.sendInput(req); err != nil {
			return err
		}
	}
}

func (s *server) sendInput(req *api.InputRequest) error {
	if req == nil {
		return status.Error(codes.InvalidArgument, "instance id is required")
	}
	id, err := requireID(req.GetId())
	if err != nil {
		return err
	}
	var ev tebox.InputEvent
	switch {
	case req.GetTouch() != nil:
		t := req.GetTouch()
		ev = tebox.InputEvent{Type: "touch", X: float64(t.GetX()), Y: float64(t.GetY())}
		switch t.GetAction() {
		case api.TouchAction_TOUCH_ACTION_DOWN:
			ev.Action = "down"
		case api.TouchAction_TOUCH_ACTION_MOVE:
			ev.Action = "move"
		case api.TouchAction_TOUCH_ACTION_UP:
			ev.Action = "up"
		default:
			return status.Error(codes.InvalidArgument, "touch action is required")
		}
	case req.GetKey() != nil:
		ev = tebox.InputEvent{Type: "key", Key: req.GetKey().GetKey()}
	case req.GetText() != nil:
		ev = tebox.InputEvent{Type: "text", Text: req.GetText().GetText()}
	case req.GetKeycode() != nil:
		k := req.GetKeycode()
		ev = tebox.InputEvent{Type: "keycode", Keycode: int(k.GetKeycode()), Meta: int(k.GetMeta()), Action: "press"}
		switch k.GetAction() {
		case api.KeyAction_KEY_ACTION_DOWN:
			ev.Action = "down"
		case api.KeyAction_KEY_ACTION_UP:
			ev.Action = "up"
		}
	case req.GetClipboard() != nil:
		c := req.GetClipboard()
		ev = tebox.InputEvent{Type: "clipboard", Text: c.GetText(), Paste: c.GetPaste()}
	default:
		return status.Error(codes.InvalidArgument, "an input event is required")
	}
	if err := s.mgr.SendInput(id, ev); err != nil {
		if err == tebox.ErrInputNotRunning {
			return status.Error(codes.FailedPrecondition, err.Error())
		}
		return status.Error(codes.InvalidArgument, err.Error())
	}
	return nil
}

func (s *server) WatchClipboard(req *api.InstanceRequest, stream api.AndroidService_WatchClipboardServer) error {
	id, err := requireID(req.GetId())
	if err != nil {
		return err
	}
	clips, cancel, err := s.mgr.WatchClipboard(id)
	if err != nil {
		if err == tebox.ErrInputNotRunning {
			return status.Error(codes.FailedPrecondition, err.Error())
		}
		return status.Error(codes.Unavailable, err.Error())
	}
	defer cancel()
	for {
		select {
		case <-stream.Context().Done():
			return nil
		case text, ok := <-clips:
			if !ok {
				return nil
			}
			if err := stream.Send(&api.ClipboardEvent{Text: text}); err != nil {
				return err
			}
		}
	}
}
