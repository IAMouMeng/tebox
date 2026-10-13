package tebox

import (
	"context"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"syscall"
	"time"
)

const (
	StateStopped  = "stopped"
	StateStarting = "starting"
	StateRunning  = "running"
	StateStopping = "stopping"

	DefaultCPUCores = 4
	DefaultMemoryMB = 4096
	DefaultDiskMB   = 8192

	// Old instances were created before sizes were stored; boot-qemu.sh
	// then used SMP=4, MEM=4096 and USERDATA_MB=1024.
	legacyDiskMB = 1024
)

// ErrInvalidOptions wraps CreateOptions validation failures.
var ErrInvalidOptions = errors.New("invalid instance options")

func (o *CreateOptions) normalize() error {
	if o.CPUCores == 0 {
		o.CPUCores = DefaultCPUCores
	}
	if o.MemoryMB == 0 {
		o.MemoryMB = DefaultMemoryMB
	}
	if o.DiskMB == 0 {
		o.DiskMB = DefaultDiskMB
	}
	if o.CPUCores < 1 || o.CPUCores > runtime.NumCPU() {
		return fmt.Errorf("%w: cpu_cores must be 1..%d (host cores)", ErrInvalidOptions, runtime.NumCPU())
	}
	if o.MemoryMB < 1024 || o.MemoryMB > 65536 {
		return fmt.Errorf("%w: memory_mb must be 1024..65536", ErrInvalidOptions)
	}
	if o.DiskMB < 1024 || o.DiskMB > 1024*1024 {
		return fmt.Errorf("%w: disk_mb must be 1024..1048576", ErrInvalidOptions)
	}
	o.Name = strings.TrimSpace(o.Name)
	return nil
}

// fillResources applies defaults for old metadata and measures disk use.
func (item *Instance) fillResources() {
	if item.CPUCores <= 0 {
		item.CPUCores = DefaultCPUCores
	}
	if item.MemoryMB <= 0 {
		item.MemoryMB = DefaultMemoryMB
	}
	img := filepath.Join(item.Dir, "userdata.img")
	st, err := os.Stat(img)
	if item.DiskMB <= 0 {
		item.DiskMB = legacyDiskMB
		if err == nil {
			item.DiskMB = int(st.Size() >> 20)
		}
	}
	if err == nil {
		used := st.Size()
		if sys, ok := st.Sys().(*syscall.Stat_t); ok {
			used = int64(sys.Blocks) * 512
		}
		item.DiskUsedMB = int((used + 1<<20 - 1) >> 20)
	}
	if item.State == "" {
		item.State = StateStopped
	}
}

// watchBoot marks the instance running once Android reports
// sys.boot_completed=1 over adb, polling until then or until this QEMU
// process is no longer the instance's.
func (m *Manager) watchBoot(id string, cmd *exec.Cmd, target string) {
	adb := adbBinary()
	for {
		time.Sleep(2 * time.Second)
		m.mu.Lock()
		current := m.procs[id] == cmd
		m.mu.Unlock()
		if !current {
			return
		}
		ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
		_ = exec.CommandContext(ctx, adb, "connect", target).Run()
		out, err := exec.CommandContext(ctx, adb, "-s", target, "shell", "getprop", "sys.boot_completed").Output()
		cancel()
		if err == nil && strings.TrimSpace(string(out)) == "1" {
			m.mu.Lock()
			if m.procs[id] == cmd {
				m.booted[id] = true
			}
			m.mu.Unlock()
			return
		}
	}
}
