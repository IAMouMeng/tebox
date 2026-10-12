package main

import (
	"bufio"
	"context"
	"errors"
	"flag"
	"fmt"
	"io"
	"net"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
	api "tebox-api/api/v1"
	tebox "tebox-api/internal/tebox"
)

type server struct {
	api.UnimplementedAndroidServiceServer
	mgr *tebox.Manager
}

func (s *server) ListInstances(ctx context.Context, _ *api.ListInstancesRequest) (*api.ListInstancesResponse, error) {
	items, err := s.mgr.List()
	if err != nil {
		return nil, internalError(err)
	}
	result := &api.ListInstancesResponse{Instances: make([]*api.Instance, 0, len(items))}
	for _, item := range items {
		result.Instances = append(result.Instances, instanceMessage(item))
	}
	return result, nil
}

func (s *server) CreateInstance(_ context.Context, req *api.CreateInstanceRequest) (*api.Instance, error) {
	item, err := s.mgr.Create(tebox.CreateOptions{
		Name: req.GetName(), CPUCores: int(req.GetCpuCores()),
		MemoryMB: int(req.GetMemoryMb()), DiskMB: int(req.GetDiskMb()),
	})
	if err != nil {
		if errors.Is(err, tebox.ErrInvalidOptions) {
			return nil, status.Error(codes.InvalidArgument, err.Error())
		}
		return nil, internalError(err)
	}
	return instanceMessage(item), nil
}

func (s *server) DeleteInstance(_ context.Context, req *api.InstanceRequest) (*api.Empty, error) {
	if req == nil {
		return nil, status.Error(codes.InvalidArgument, "instance id is required")
	}
	id, err := requireID(req.GetId())
	if err != nil {
		return nil, err
	}
	if err := s.mgr.Delete(id); err != nil {
		return nil, internalError(err)
	}
	return &api.Empty{}, nil
}

func (s *server) StartInstance(_ context.Context, req *api.InstanceRequest) (*api.Empty, error) {
	if req == nil {
		return nil, status.Error(codes.InvalidArgument, "instance id is required")
	}
	id, err := requireID(req.GetId())
	if err != nil {
		return nil, err
	}
	if err := s.mgr.Start(id); err != nil {
		return nil, internalError(err)
	}
	return &api.Empty{}, nil
}

func (s *server) StopInstance(_ context.Context, req *api.InstanceRequest) (*api.Empty, error) {
	if req == nil {
		return nil, status.Error(codes.InvalidArgument, "instance id is required")
	}
	id, err := requireID(req.GetId())
	if err != nil {
		return nil, err
	}
	if err := s.mgr.Stop(id); err != nil {
		return nil, internalError(err)
	}
	return &api.Empty{}, nil
}

func (s *server) ResetInstance(_ context.Context, req *api.ResetInstanceRequest) (*api.Empty, error) {
	if req == nil {
		return nil, status.Error(codes.InvalidArgument, "instance id is required")
	}
	id, err := requireID(req.GetId())
	if err != nil {
		return nil, err
	}
	if err := s.mgr.Reset(id, req.GetWipeData()); err != nil {
		return nil, internalError(err)
	}
	return &api.Empty{}, nil
}

func (s *server) ConfigureInstance(_ context.Context, req *api.ConfigureInstanceRequest) (*api.Empty, error) {
	if req == nil {
		return nil, status.Error(codes.InvalidArgument, "instance id is required")
	}
	id, err := requireID(req.GetId())
	if err != nil {
		return nil, err
	}
	if req.Profile == nil {
		return nil, status.Error(codes.InvalidArgument, "profile is required")
	}
	p := req.Profile
	err = s.mgr.Configure(id, tebox.DeviceProfile{Latitude: p.Latitude, Longitude: p.Longitude, Country: p.Country, OperatorNumeric: p.OperatorNumeric, OperatorName: p.OperatorName, CellId: p.CellId, PhoneNumber: p.PhoneNumber, Root: p.Root})
	if err != nil {
		return nil, internalError(err)
	}
	return &api.Empty{}, nil
}

func (s *server) SendKey(_ context.Context, req *api.KeyRequest) (*api.Empty, error) {
	if req == nil {
		return nil, status.Error(codes.InvalidArgument, "instance id is required")
	}
	id, err := requireID(req.GetId())
	if err != nil {
		return nil, err
	}
	path, err := s.mgr.QMPPath(id)
	if err != nil {
		return nil, internalError(err)
	}
	if err = tebox.SendKey(path, req.GetKey()); err != nil {
		return nil, internalError(err)
	}
	return &api.Empty{}, nil
}

func (s *server) PowerDown(_ context.Context, req *api.InstanceRequest) (*api.Empty, error) {
	if req == nil {
		return nil, status.Error(codes.InvalidArgument, "instance id is required")
	}
	id, err := requireID(req.GetId())
	if err != nil {
		return nil, err
	}
	path, err := s.mgr.QMPPath(id)
	if err != nil {
		return nil, internalError(err)
	}
	if err = tebox.PowerDown(path); err != nil {
		return nil, internalError(err)
	}
	return &api.Empty{}, nil
}

func requireID(raw string) (string, error) {
	id := strings.TrimSpace(raw)
	if id == "" || id == "." || id == ".." || strings.ContainsAny(id, `/\\`) {
		return "", status.Error(codes.InvalidArgument, "invalid instance id")
	}
	return id, nil
}

func instanceMessage(item tebox.Instance) *api.Instance {
	return &api.Instance{
		Id: item.ID, Name: item.Name, Variant: item.Variant, Dir: item.Dir,
		AdbPort: int32(item.AdbPort), Pid: int32(item.PID), Running: item.Running,
		CastPort: int32(item.CastPort), ControlPort: int32(item.ControlPort),
		CpuCores: int32(item.CPUCores), MemoryMb: int32(item.MemoryMB),
		DiskMb: int32(item.DiskMB), DiskUsedMb: int32(item.DiskUsedMB),
		State: instanceState(item.State),
		Profile: &api.DeviceProfile{
			Latitude: item.Profile.Latitude, Longitude: item.Profile.Longitude,
			Country: item.Profile.Country, OperatorNumeric: item.Profile.OperatorNumeric,
			OperatorName: item.Profile.OperatorName, CellId: item.Profile.CellId,
			PhoneNumber: item.Profile.PhoneNumber, Root: item.Profile.Root,
		},
	}
}

func instanceState(state string) api.InstanceState {
	switch state {
	case tebox.StateStopped:
		return api.InstanceState_INSTANCE_STATE_STOPPED
	case tebox.StateStarting:
		return api.InstanceState_INSTANCE_STATE_STARTING
	case tebox.StateRunning:
		return api.InstanceState_INSTANCE_STATE_RUNNING
	case tebox.StateStopping:
		return api.InstanceState_INSTANCE_STATE_STOPPING
	}
	return api.InstanceState_INSTANCE_STATE_UNSPECIFIED
}

func internalError(err error) error {
	if errors.Is(err, os.ErrNotExist) {
		return status.Error(codes.NotFound, err.Error())
	}
	return status.Error(codes.Internal, err.Error())
}

func main() {
	addr := flag.String("listen", getenv("TEBOX_GRPC_ADDR", "127.0.0.1:50051"), "gRPC listen address")
	castAddr := flag.String("cast-listen", getenv("TEBOX_CAST_ADDR", "127.0.0.1:9901"), "multiplexed cast listen address")
	autostart := flag.String("start", getenv("TEBOX_AUTOSTART", ""), "comma-separated instance ids to start once serving")
	inputAddr := flag.String("input-udp", getenv("TEBOX_INPUT_UDP_ADDR", "127.0.0.1:9903"), "shared UDP input address (events carry id/ids)")
	flag.Parse()
	root, err := tebox.ResolveProjectRoot()
	if err != nil {
		fatal(err)
	}
	mgr, err := tebox.NewManager(root, tebox.DefaultDataRoot())
	if err != nil {
		fatal(err)
	}
	listener, err := net.Listen("tcp", *addr)
	if err != nil {
		fatal(err)
	}
	grpcServer := grpc.NewServer()
	api.RegisterAndroidServiceServer(grpcServer, &server{mgr: mgr})
	castListener, err := net.Listen("tcp", *castAddr)
	if err != nil {
		fatal(err)
	}
	go serveCast(castListener, mgr)
	if err := mgr.ServeInputUDP(*inputAddr); err != nil {
		fatal(err)
	}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	for _, id := range strings.Split(*autostart, ",") {
		if id = strings.TrimSpace(id); id != "" {
			if err := mgr.Start(id); err != nil {
				fmt.Fprintf(os.Stderr, "autostart %s: %v\n", id, err)
			}
		}
	}
	go func() {
		<-ctx.Done()
		grpcServer.GracefulStop()
		mgr.ShutdownRunning()
	}()
	fmt.Printf("tebox gRPC listening on %s, cast %s, input udp %s (root=%s, data=%s)\n",
		*addr, *castAddr, *inputAddr, root, mgr.DataRoot)
	if err := grpcServer.Serve(listener); err != nil && ctx.Err() == nil {
		fatal(err)
	}
}

func serveCast(listener net.Listener, mgr *tebox.Manager) {
	for {
		conn, err := listener.Accept()
		if err != nil {
			return
		}
		go handleCast(conn, mgr)
	}
}

func handleCast(conn net.Conn, mgr *tebox.Manager) {
	defer conn.Close()
	_ = conn.SetReadDeadline(time.Now().Add(5 * time.Second))
	reader := bufio.NewReader(conn)
	line, err := reader.ReadString('\n')
	if err != nil {
		return
	}
	parts := strings.Fields(strings.TrimSpace(line))
	if len(parts) == 2 && parts[0] == "TEBOX-CTRL/1" {
		_ = conn.SetReadDeadline(time.Time{})
		handleControl(conn, reader, mgr, parts[1])
		return
	}
	if len(parts) != 2 || parts[0] != "TEBOX-CAST/1" {
		return
	}
	port, err := mgr.CastPort(parts[1])
	if err != nil {
		return
	}
	backend, err := net.DialTimeout("tcp", fmt.Sprintf("127.0.0.1:%d", port), 2*time.Second)
	if err != nil {
		return
	}
	defer backend.Close()
	_ = conn.SetReadDeadline(time.Time{})
	done := make(chan struct{}, 2)
	go func() { _, _ = io.Copy(conn, backend); done <- struct{}{} }()
	go func() { _, _ = io.Copy(backend, conn); done <- struct{}{} }()
	<-done
}

func getenv(key, fallback string) string {
	if value := os.Getenv(key); value != "" {
		return value
	}
	return fallback
}

func fatal(err error) {
	fmt.Fprintln(os.Stderr, err)
	os.Exit(1)
}
