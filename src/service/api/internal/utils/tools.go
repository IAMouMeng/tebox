package utils

import (
	"fmt"
	"net"
	"os"
	"path/filepath"
	"strings"

	"tebox-api/internal/constants"
)

// ADBBinary returns TEBOX_ADB or the default adb executable name.
func ADBBinary() string {
	if adb := os.Getenv(constants.EnvADB); adb != "" {
		return adb
	}
	return constants.DefaultADBBinary
}

// ResolveProjectRoot returns TEBOX_ROOT or the process working directory.
func ResolveProjectRoot() (string, error) {
	if root := os.Getenv(constants.EnvRoot); root != "" {
		return root, nil
	}
	return os.Getwd()
}

// DefaultDataRoot returns TEBOX_DATA_DIR or ~/.tebox.
func DefaultDataRoot() string {
	if data := os.Getenv(constants.EnvDataDir); data != "" {
		return data
	}
	home, err := os.UserHomeDir()
	if err != nil || home == "" {
		return ".tebox"
	}
	return filepath.Join(home, ".tebox")
}

// ChooseVariant picks TEBOX_VARIANT (if it has KERNEL) or the first src/aosp/* with KERNEL.
func ChooseVariant(root string) (string, error) {
	if preferred := os.Getenv(constants.EnvVariant); preferred != "" {
		if _, err := os.Stat(filepath.Join(root, "src", "aosp", preferred, "KERNEL")); err == nil {
			return preferred, nil
		}
	}
	entries, err := os.ReadDir(filepath.Join(root, "src", "aosp"))
	if err != nil {
		return "", fmt.Errorf("no Android variant under %s/src/aosp", root)
	}
	for _, entry := range entries {
		if !entry.IsDir() || strings.HasPrefix(entry.Name(), ".") {
			continue
		}
		if _, err := os.Stat(filepath.Join(root, "src", "aosp", entry.Name(), "KERNEL")); err == nil {
			return entry.Name(), nil
		}
	}
	return "", fmt.Errorf("no Android variant under %s/src/aosp", root)
}

// TCPPortFree reports whether 127.0.0.1:port can be bound.
func TCPPortFree(port int) bool {
	ln, err := net.Listen("tcp", fmt.Sprintf("127.0.0.1:%d", port))
	if err != nil {
		return false
	}
	_ = ln.Close()
	return true
}

// AllocateTCPPort finds a free port at or after start, skipping values in used.
func AllocateTCPPort(start int, used map[string]int) (int, error) {
	busy := map[int]bool{}
	for _, p := range used {
		busy[p] = true
	}
	for port := start; port < 65535; port++ {
		if !busy[port] && TCPPortFree(port) {
			return port, nil
		}
	}
	return 0, fmt.Errorf("no free cast port available")
}

// InputQMPPath is the per-instance QMP input monitor socket.
func InputQMPPath(dir string) string {
	return filepath.Join(dir, "qmp-input.sock")
}

// MonitorQMPPath is the per-instance primary QMP monitor socket.
func MonitorQMPPath(dir string) string {
	return filepath.Join(dir, "qmp.sock")
}
