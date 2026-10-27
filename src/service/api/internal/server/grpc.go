package server

import (
	"context"

	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
	api "tebox-api/api/v1"
	"tebox-api/internal/service"
	"tebox-api/internal/service/types"
	"tebox-api/internal/utils"
)

// Service is the gRPC AndroidService implementation.
type Service struct {
	api.UnimplementedAndroidServiceServer
	mgr *service.Manager
}

// New returns the gRPC service backed by mgr.
func New(mgr *service.Manager) *Service {
	return &Service{mgr: mgr}
}

func (s *Service) ListInstances(context.Context, *api.ListInstancesRequest) (*api.ListInstancesResponse, error) {
	items, err := s.mgr.List()
	if err != nil {
		return nil, utils.RPCError(err)
	}
	result := &api.ListInstancesResponse{Instances: make([]*api.Instance, 0, len(items))}
	for _, item := range items {
		result.Instances = append(result.Instances, instanceMessage(item))
	}
	return result, nil
}

func (s *Service) GetInstance(_ context.Context, req *api.InstanceRequest) (*api.Instance, error) {
	id, err := utils.RequireID(req.GetId())
	if err != nil {
		return nil, err
	}
	item, err := s.mgr.Get(id)
	if err != nil {
		return nil, utils.RPCError(err)
	}
	return instanceMessage(item), nil
}

func (s *Service) CreateInstance(_ context.Context, req *api.CreateInstanceRequest) (*api.Instance, error) {
	item, err := s.mgr.Create(types.CreateOptions{
		Name:           req.GetName(),
		CPUCores:       int(req.GetCpuCores()),
		MemoryMB:       int(req.GetMemoryMb()),
		DiskMB:         int(req.GetDiskMb()),
		SystemImage:    req.GetSystemImage(),
		VendorImage:    req.GetVendorImage(),
		InitramfsImage: req.GetInitramfsImage(),
		InstanceDir:    req.GetInstanceDir(),
	})
	if err != nil {
		return nil, utils.RPCError(err)
	}
	return instanceMessage(item), nil
}

func (s *Service) DeleteInstance(_ context.Context, req *api.InstanceRequest) (*api.Empty, error) {
	id, err := utils.RequireID(req.GetId())
	if err != nil {
		return nil, err
	}
	if err := s.mgr.Delete(id); err != nil {
		return nil, utils.RPCError(err)
	}
	return &api.Empty{}, nil
}

func (s *Service) StartInstance(_ context.Context, req *api.InstanceRequest) (*api.Empty, error) {
	id, err := utils.RequireID(req.GetId())
	if err != nil {
		return nil, err
	}
	if err := s.mgr.Start(id); err != nil {
		return nil, utils.RPCError(err)
	}
	return &api.Empty{}, nil
}

func (s *Service) StopInstance(_ context.Context, req *api.InstanceRequest) (*api.Empty, error) {
	id, err := utils.RequireID(req.GetId())
	if err != nil {
		return nil, err
	}
	if err := s.mgr.Stop(id); err != nil {
		return nil, utils.RPCError(err)
	}
	return &api.Empty{}, nil
}

func (s *Service) ResetInstance(_ context.Context, req *api.ResetInstanceRequest) (*api.Empty, error) {
	id, err := utils.RequireID(req.GetId())
	if err != nil {
		return nil, err
	}
	if err := s.mgr.Reset(id, req.GetWipeData()); err != nil {
		return nil, utils.RPCError(err)
	}
	return &api.Empty{}, nil
}

func (s *Service) ConfigureInstance(_ context.Context, req *api.ConfigureInstanceRequest) (*api.Empty, error) {
	id, err := utils.RequireID(req.GetId())
	if err != nil {
		return nil, err
	}
	if req.GetProfile() == nil {
		return nil, status.Error(codes.InvalidArgument, "profile is required")
	}
	if err := s.mgr.Configure(id, deviceProfile(req.GetProfile())); err != nil {
		return nil, utils.RPCError(err)
	}
	return &api.Empty{}, nil
}

func (s *Service) SendKey(_ context.Context, req *api.KeyRequest) (*api.Empty, error) {
	id, err := utils.RequireID(req.GetId())
	if err != nil {
		return nil, err
	}
	qmp, err := s.monitor(id)
	if err != nil {
		return nil, err
	}
	if err := qmp.SendKey(req.GetKey()); err != nil {
		return nil, utils.RPCError(err)
	}
	return &api.Empty{}, nil
}

func (s *Service) PowerDown(_ context.Context, req *api.InstanceRequest) (*api.Empty, error) {
	id, err := utils.RequireID(req.GetId())
	if err != nil {
		return nil, err
	}
	qmp, err := s.monitor(id)
	if err != nil {
		return nil, err
	}
	if err := qmp.PowerDown(); err != nil {
		return nil, utils.RPCError(err)
	}
	return &api.Empty{}, nil
}

func (s *Service) GetPreview(_ context.Context, req *api.GetPreviewRequest) (*api.GetPreviewResponse, error) {
	id, err := utils.RequireID(req.GetId())
	if err != nil {
		return nil, err
	}
	preview, err := s.mgr.Preview(id, types.PreviewOptions{
		Size:    int(req.GetSize()),
		Format:  req.GetFormat(),
		Quality: int(req.GetQuality()),
	})
	if err != nil {
		return nil, utils.RPCError(err)
	}
	return &api.GetPreviewResponse{
		Image:    preview.Data,
		Format:   preview.Format,
		MimeType: preview.Mime,
		Width:    int32(preview.Width),
		Height:   int32(preview.Height),
	}, nil
}

func (s *Service) monitor(id string) (utils.QMPSocket, error) {
	path, err := s.mgr.QMPPath(id)
	if err != nil {
		return utils.QMPSocket{}, utils.RPCError(err)
	}
	return utils.QMPSocket{Path: path}, nil
}

func instanceMessage(item types.Instance) *api.Instance {
	return &api.Instance{
		Id:             item.ID,
		Name:           item.Name,
		Variant:        item.Variant,
		Dir:            item.Dir,
		AdbPort:        int32(item.AdbPort),
		Pid:            int32(item.PID),
		Running:        item.Running,
		CastPort:       int32(item.CastPort),
		ControlPort:    int32(item.ControlPort),
		CpuCores:       int32(item.CPUCores),
		MemoryMb:       int32(item.MemoryMB),
		DiskMb:         int32(item.DiskMB),
		DiskUsedMb:     int32(item.DiskUsedMB),
		DiskFreeMb:     utils.DiskFreeMB(item.DiskMB, item.DiskUsedMB),
		State:          item.State,
		SystemImage:    item.SystemImage,
		VendorImage:    item.VendorImage,
		InitramfsImage: item.InitramfsImage,
		Profile:        profileMessage(item.Profile),
	}
}

func profileMessage(p types.DeviceProfile) *api.DeviceProfile {
	return &api.DeviceProfile{
		Latitude:        p.Latitude,
		Longitude:       p.Longitude,
		Country:         p.Country,
		OperatorNumeric: p.OperatorNumeric,
		OperatorName:    p.OperatorName,
		CellId:          p.CellId,
		PhoneNumber:     p.PhoneNumber,
		Root:            p.Root,
	}
}

func deviceProfile(p *api.DeviceProfile) types.DeviceProfile {
	return types.DeviceProfile{
		Latitude:        p.GetLatitude(),
		Longitude:       p.GetLongitude(),
		Country:         p.GetCountry(),
		OperatorNumeric: p.GetOperatorNumeric(),
		OperatorName:    p.GetOperatorName(),
		CellId:          p.GetCellId(),
		PhoneNumber:     p.GetPhoneNumber(),
		Root:            p.GetRoot(),
	}
}
