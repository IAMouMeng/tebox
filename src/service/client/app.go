package main

import (
	"context"
	"fmt"

	"tebox-client/internal/tebox"
)

type App struct {
	ctx context.Context
	mgr *tebox.Manager
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
	root, err := tebox.ResolveProjectRoot()
	if err != nil {
		return
	}
	mgr, err := tebox.NewManager(root, tebox.DefaultDataRoot())
	if err != nil {
		return
	}
	a.mgr = mgr
}

func (a *App) shutdown(ctx context.Context) {
	_ = ctx
	if a.mgr != nil {
		a.mgr.ShutdownRunning()
	}
}

func (a *App) ensure() error {
	if a.mgr == nil {
		return fmt.Errorf("tebox is not initialized (set TEBOX_ROOT)")
	}
	return nil
}

func (a *App) GetStatus() (Status, error) {
	if err := a.ensure(); err != nil {
		return Status{}, err
	}
	return Status{
		Variant:  a.mgr.Variant,
		DataRoot: a.mgr.DataRoot,
		Root:     a.mgr.ProjectRoot,
	}, nil
}

func (a *App) ListInstances() ([]tebox.Instance, error) {
	if err := a.ensure(); err != nil {
		return nil, err
	}
	return a.mgr.List()
}

func (a *App) CreateInstance() (tebox.Instance, error) {
	if err := a.ensure(); err != nil {
		return tebox.Instance{}, err
	}
	return a.mgr.Create()
}

func (a *App) DeleteInstance(id string) error {
	if err := a.ensure(); err != nil {
		return err
	}
	return a.mgr.Delete(id)
}

func (a *App) StartInstance(id string) error {
	if err := a.ensure(); err != nil {
		return err
	}
	return a.mgr.Start(id)
}

func (a *App) StopInstance(id string) error {
	if err := a.ensure(); err != nil {
		return err
	}
	return a.mgr.Stop(id)
}

func (a *App) ConfigureInstance(id string, profile tebox.DeviceProfile) error {
	if err := a.ensure(); err != nil {
		return err
	}
	return a.mgr.Configure(id, profile)
}

func (a *App) SendKey(id, key string) error {
	if err := a.ensure(); err != nil {
		return err
	}
	path, err := a.mgr.QMPPath(id)
	if err != nil {
		return err
	}
	return tebox.SendKey(path, key)
}

func (a *App) PowerDown(id string) error {
	if err := a.ensure(); err != nil {
		return err
	}
	path, err := a.mgr.QMPPath(id)
	if err != nil {
		return err
	}
	return tebox.PowerDown(path)
}
