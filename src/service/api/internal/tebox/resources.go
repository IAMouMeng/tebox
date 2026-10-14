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
	if o.InstanceDir = strings.TrimSpace(o.InstanceDir); o.InstanceDir != "" {
		if err := checkInstanceDir(&o.InstanceDir); err != nil {
			return fmt.Errorf("%w: %v", ErrInvalidOptions, err)
		}
	}
	for _, img := range []*string{&o.SystemImage, &o.VendorImage, &o.InitramfsImage} {
		if *img = strings.TrimSpace(*img); *img == "" {
			continue
		}
		if !filepath.IsAbs(*img) {
			return fmt.Errorf("%w: image path must be absolute: %s", ErrInvalidOptions, *img)
		}
		*img = filepath.Clean(*img)
		if err := checkImage(*img); err != nil {
			return fmt.Errorf("%w: %v", ErrInvalidOptions, err)
		}
	}
	return nil
}

// Unix socket paths (sun_path) hold 104 bytes on macOS, 108 on Linux; the
// longest socket in an instance directory is "/qmp-input.sock".
const maxInstanceDirLen = 103 - len("/qmp-input.sock")

// checkInstanceDir requires an absolute path whose sockets fit sun_path and
// that is absent or an empty directory, so Delete only removes what the
// instance created.
func checkInstanceDir(dir *string) error {
	if !filepath.IsAbs(*dir) {
		return fmt.Errorf("instance_dir must be absolute: %s", *dir)
	}
	*dir = filepath.Clean(*dir)
	if *dir == "/" {
		return fmt.Errorf("instance_dir cannot be /")
	}
	if len(*dir) > maxInstanceDirLen {
		return fmt.Errorf("instance_dir is %d bytes; at most %d fit the instance's Unix sockets", len(*dir), maxInstanceDirLen)
	}
	entries, err := os.ReadDir(*dir)
	if err != nil {
		if os.IsNotExist(err) {
			return nil
		}
		return fmt.Errorf("instance_dir %s: %w", *dir, err)
	}
	if len(entries) > 0 {
		return fmt.Errorf("instance_dir %s is not empty", *dir)
	}
	return nil
}

// checkImage requires a non-empty regular file the API can read.
func checkImage(path string) error {
	st, err := os.Stat(path)
	if err != nil {
		return fmt.Errorf("image %s: %w", path, err)
	}
	if !st.Mode().IsRegular() || st.Size() == 0 {
		return fmt.Errorf("image %s is not a non-empty file", path)
	}
	f, err := os.Open(path)
	if err != nil {
		return fmt.Errorf("image %s: %w", path, err)
	}
	return f.Close()
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
		item.DiskUsedMB = usedMB(img)
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

// usedMB is the host space a (sparse or cloned) file occupies.
func usedMB(path string) int {
	st, err := os.Stat(path)
	if err != nil {
		return 0
	}
	used := st.Size()
	if sys, ok := st.Sys().(*syscall.Stat_t); ok {
		used = int64(sys.Blocks) * 512
	}
	return int((used + 1<<20 - 1) >> 20)
}
