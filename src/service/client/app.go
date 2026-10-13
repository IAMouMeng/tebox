package main

import (
	"context"
	"fmt"
	"os"

	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
	api "tebox-client/api/v1"
	"tebox-client/internal/tebox"
)

type App struct {
	ctx  context.Context
	conn *grpc.ClientConn
	api  api.AndroidServiceClient
}

type Status struct {
	Variant  string `json:"variant"`
	DataRoot string `json:"dataRoot"`
	Root     string `json:"root"`
}

func NewApp() *App {
	return &App{}
}

func (a *App) startup(ctx context.Context) {
	a.ctx = ctx
	addr := os.Getenv("TEBOX_API_ADDR")
	if addr == "" {
		addr = "127.0.0.1:50051"
	}
	conn, err := grpc.Dial(addr, grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		return
	}
	a.conn, a.api = conn, api.NewAndroidServiceClient(conn)
}

func (a *App) shutdown(ctx context.Context) {
	_ = ctx
	if a.conn != nil {
		_ = a.conn.Close()
	}
}

func (a *App) ensure() error {
	if a.api == nil {
		return fmt.Errorf("tebox API is not connected (set TEBOX_API_ADDR and start tebox-api)")
	}
	return nil
}

func (a *App) GetStatus() (Status, error) {
	if err := a.ensure(); err != nil {
		return Status{}, err
	}
	return Status{
		Variant: os.Getenv("TEBOX_VARIANT"), DataRoot: os.Getenv("TEBOX_DATA_DIR"), Root: os.Getenv("TEBOX_ROOT"),
	}, nil
}

func (a *App) ListInstances() ([]tebox.Instance, error) {
	if err := a.ensure(); err != nil {
		return nil, err
	}
	res, err := a.api.ListInstances(a.ctx, &api.ListInstancesRequest{})
	if err != nil {
		return nil, err
	}
	return fromMessages(res.Instances), nil
}

func (a *App) CreateInstance() (tebox.Instance, error) {
	if err := a.ensure(); err != nil {
		return tebox.Instance{}, err
	}
	item, err := a.api.CreateInstance(a.ctx, &api.CreateInstanceRequest{})
	if err != nil {
		return tebox.Instance{}, err
	}
	return fromMessage(item), nil
}

func (a *App) DeleteInstance(id string) error {
	if err := a.ensure(); err != nil {
		return err
	}
	_, err := a.api.DeleteInstance(a.ctx, &api.InstanceRequest{Id: id})
	return err
}

func (a *App) StartInstance(id string) error {
	if err := a.ensure(); err != nil {
		return err
	}
	_, err := a.api.StartInstance(a.ctx, &api.InstanceRequest{Id: id})
	return err
}

func (a *App) StopInstance(id string) error {
	if err := a.ensure(); err != nil {
		return err
	}
	_, err := a.api.StopInstance(a.ctx, &api.InstanceRequest{Id: id})
	return err
}

func (a *App) ConfigureInstance(id string, profile tebox.DeviceProfile) error {
	if err := a.ensure(); err != nil {
		return err
	}
	_, err := a.api.ConfigureInstance(a.ctx, &api.ConfigureInstanceRequest{Id: id, Profile: &api.DeviceProfile{Latitude: profile.Latitude, Longitude: profile.Longitude, Country: profile.Country, OperatorNumeric: profile.OperatorNumeric, OperatorName: profile.OperatorName, CellId: profile.CellId, PhoneNumber: profile.PhoneNumber, Root: profile.Root}})
	return err
}

func (a *App) SendKey(id, key string) error {
	if err := a.ensure(); err != nil {
		return err
	}
	_, err := a.api.SendKey(a.ctx, &api.KeyRequest{Id: id, Key: key})
	return err
}

func (a *App) PowerDown(id string) error {
	if err := a.ensure(); err != nil {
		return err
	}
	_, err := a.api.PowerDown(a.ctx, &api.InstanceRequest{Id: id})
	return err
}

func (a *App) ResetInstance(id string, wipeData bool) error {
	if err := a.ensure(); err != nil {
		return err
	}
	_, err := a.api.ResetInstance(a.ctx, &api.ResetInstanceRequest{Id: id, WipeData: wipeData})
	return err
}

func fromMessages(items []*api.Instance) []tebox.Instance {
	out := make([]tebox.Instance, 0, len(items))
	for _, i := range items {
		out = append(out, fromMessage(i))
	}
	return out
}
func fromMessage(i *api.Instance) tebox.Instance {
	p := tebox.DeviceProfile{}
	if i.Profile != nil {
		p = tebox.DeviceProfile{Latitude: i.Profile.Latitude, Longitude: i.Profile.Longitude, Country: i.Profile.Country, OperatorNumeric: i.Profile.OperatorNumeric, OperatorName: i.Profile.OperatorName, CellId: i.Profile.CellId, PhoneNumber: i.Profile.PhoneNumber, Root: i.Profile.Root}
	}
	return tebox.Instance{ID: i.Id, Name: i.Name, Variant: i.Variant, Dir: i.Dir, AdbPort: int(i.AdbPort), PID: int(i.Pid), Running: i.Running, Profile: p}
}
