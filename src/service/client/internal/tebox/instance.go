package tebox

import (
	"encoding/json"
	"fmt"
	"hash/fnv"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"time"
)

type Instance struct {
	ID      string        `json:"id"`
	Name    string        `json:"name"`
	Variant string        `json:"variant"`
	Dir     string        `json:"dir"`
	AdbPort int           `json:"adbPort"`
	PID     int           `json:"pid"`
	Running bool          `json:"running"`
	Profile DeviceProfile `json:"profile"`
}

type DeviceProfile struct {
	Latitude        float64 `json:"latitude"`
	Longitude       float64 `json:"longitude"`
	Country         string  `json:"country"`
	OperatorNumeric string  `json:"operatorNumeric"`
	OperatorName    string  `json:"operatorName"`
	CellId          string  `json:"cellId"`
	PhoneNumber     string  `json:"phoneNumber"`
	Root            bool    `json:"root"`
}

type Manager struct {
	ProjectRoot string
	DataRoot    string
	Variant     string

	mu    sync.Mutex
	procs map[string]*exec.Cmd
}

func NewManager(projectRoot, dataRoot string) (*Manager, error) {
	variant, err := ChooseVariant(projectRoot)
	if err != nil {
		return nil, err
	}
	m := &Manager{
		ProjectRoot: projectRoot,
		DataRoot:    dataRoot,
		Variant:     variant,
		procs:       make(map[string]*exec.Cmd),
	}
	if err := os.MkdirAll(m.instancesDir(), 0o755); err != nil {
		return nil, err
	}
	return m, nil
}

func ChooseVariant(root string) (string, error) {
	if preferred := os.Getenv("TEBOX_VARIANT"); preferred != "" {
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

func DefaultDataRoot() string {
	if data := os.Getenv("TEBOX_DATA_DIR"); data != "" {
		return data
	}
	home, err := os.UserHomeDir()
	if err != nil || home == "" {
		return ".tebox"
	}
	return filepath.Join(home, ".tebox")
}

func ResolveProjectRoot() (string, error) {
	if root := os.Getenv("TEBOX_ROOT"); root != "" {
		return root, nil
	}
	cwd, err := os.Getwd()
	if err != nil {
		return "", err
	}
	return cwd, nil
}

func (m *Manager) instancesDir() string {
	return filepath.Join(m.DataRoot, "instances")
}

func (m *Manager) List() ([]Instance, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.reapLocked()

	entries, err := os.ReadDir(m.instancesDir())
	if err != nil {
		if os.IsNotExist(err) {
			return nil, nil
		}
		return nil, err
	}
	out := make([]Instance, 0, len(entries))
	for _, entry := range entries {
		if !entry.IsDir() || strings.HasPrefix(entry.Name(), ".") {
			continue
		}
		item, err := m.readMetadata(filepath.Join(m.instancesDir(), entry.Name()))
		if err != nil {
			continue
		}
		if cmd := m.procs[item.ID]; cmd != nil && cmd.Process != nil {
			item.PID = cmd.Process.Pid
			item.Running = true
		}
		out = append(out, item)
	}
	return out, nil
}

func (m *Manager) Create() (Instance, error) {
	now := time.Now()
	item := Instance{
		ID:      fmt.Sprintf("android-%d-%d", now.Unix(), now.Nanosecond()/1e6),
		Name:    fmt.Sprintf("Android %d", now.Unix()),
		Variant: m.Variant,
		Profile: DeviceProfile{Latitude: 39.9042, Longitude: 116.4074, Country: "CN", OperatorNumeric: "46000", OperatorName: "China Mobile", CellId: "0x1001", PhoneNumber: "+8613800138000"},
	}
	item.Dir = filepath.Join(m.instancesDir(), item.ID)
	port, err := m.allocateAdbPort(item.ID)
	if err != nil {
		return Instance{}, err
	}
	item.AdbPort = port
	if err := os.MkdirAll(item.Dir, 0o755); err != nil {
		return Instance{}, err
	}
	if err := writeMetadata(item); err != nil {
		return Instance{}, err
	}
	return item, nil
}

func (m *Manager) Delete(id string) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	item, err := m.readMetadata(filepath.Join(m.instancesDir(), id))
	if err != nil {
		return err
	}
	if cmd := m.procs[id]; cmd != nil && cmd.Process != nil {
		_ = cmd.Process.Signal(os.Interrupt)
		delete(m.procs, id)
		time.Sleep(200 * time.Millisecond)
	}
	return os.RemoveAll(item.Dir)
}

func (m *Manager) Start(id string) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.reapLocked()
	if cmd := m.procs[id]; cmd != nil && cmd.Process != nil {
		return nil
	}
	item, err := m.readMetadata(filepath.Join(m.instancesDir(), id))
	if err != nil {
		return err
	}
	if item.AdbPort <= 0 {
		port, err := m.allocateAdbPortLocked(item.ID)
		if err != nil {
			return err
		}
		item.AdbPort = port
		if err := writeMetadata(item); err != nil {
			return err
		}
	}
	runPath := filepath.Join(m.ProjectRoot, "run")
	qmpPath := filepath.Join(item.Dir, "qmp.sock")
	_ = os.Remove(qmpPath)

	cmd := exec.Command(runPath, item.Variant)
	cmd.Dir = m.ProjectRoot
	cmd.Env = append(os.Environ(),
		"TEBOX_INSTANCE_DIR="+item.Dir,
		"QMP_SOCKET="+qmpPath,
		"SNAPSHOT=0",
		"FORCE_VIRGL=1",
		"QEMU_DEBUG=1",
		"TEBOX_WINDOW_CONTROLS=1",
		fmt.Sprintf("ADB_PORT=%d", item.AdbPort),
		"QEMU_NAME="+item.Name,
	)
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	if err := cmd.Start(); err != nil {
		return err
	}
	m.procs[id] = cmd
	go func() {
		_ = cmd.Wait()
		m.mu.Lock()
		if m.procs[id] == cmd {
			delete(m.procs, id)
		}
		m.mu.Unlock()
	}()
	return nil
}

func (m *Manager) Stop(id string) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.reapLocked()
	cmd := m.procs[id]
	if cmd == nil || cmd.Process == nil {
		return nil
	}
	path := filepath.Join(m.instancesDir(), id, "qmp.sock")
	_ = PowerDown(path)
	_ = cmd.Process.Signal(os.Interrupt)
	return nil
}

// Configure applies instance-specific guest identity/location values over ADB.
// It deliberately leaves system.img and the kernel untouched.
func (m *Manager) Configure(id string, profile DeviceProfile) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	item, err := m.readMetadata(filepath.Join(m.instancesDir(), id))
	if err != nil {
		return err
	}
	item.Profile = profile
	if err := writeMetadata(item); err != nil {
		return err
	}
	cmd := m.procs[id]
	if cmd == nil || cmd.Process == nil {
		return nil
	}
	adb := os.Getenv("TEBOX_ADB")
	if adb == "" {
		adb = "adb"
	}
	target := fmt.Sprintf("127.0.0.1:%d", item.AdbPort)
	commands := [][]string{
		{"shell", "settings", "put", "secure", "location_providers_allowed", "+gps"},
		{"shell", "setprop", "gsm.operator.iso-country", strings.ToLower(profile.Country)},
		{"shell", "setprop", "gsm.sim.operator.iso-country", strings.ToLower(profile.Country)},
		{"shell", "setprop", "gsm.operator.numeric", profile.OperatorNumeric},
		{"shell", "setprop", "gsm.sim.operator.numeric", profile.OperatorNumeric},
		{"shell", "setprop", "gsm.operator.alpha", profile.OperatorName},
		{"shell", "setprop", "gsm.sim.operator.alpha", profile.OperatorName},
		{"shell", "setprop", "gsm.sim.line1.number", profile.PhoneNumber},
		{"shell", "setprop", "gsm.line1.number", profile.PhoneNumber},
	}
	for _, args := range commands {
		full := append([]string{"-s", target}, args...)
		if out, runErr := exec.Command(adb, full...).CombinedOutput(); runErr != nil {
			return fmt.Errorf("adb %v: %w (%s)", args, runErr, strings.TrimSpace(string(out)))
		}
	}
	if profile.Root {
		if out, runErr := exec.Command(adb, "-s", target, "root").CombinedOutput(); runErr != nil {
			return fmt.Errorf("adb root: %w (%s)", runErr, strings.TrimSpace(string(out)))
		}
	}
	return nil
}

func (m *Manager) QMPPath(id string) (string, error) {
	item, err := m.readMetadata(filepath.Join(m.instancesDir(), id))
	if err != nil {
		return "", err
	}
	return filepath.Join(item.Dir, "qmp.sock"), nil
}

func (m *Manager) ShutdownRunning() {
	m.mu.Lock()
	defer m.mu.Unlock()
	for id, cmd := range m.procs {
		if cmd == nil || cmd.Process == nil {
			continue
		}
		path := filepath.Join(m.instancesDir(), id, "qmp.sock")
		_ = PowerDown(path)
	}
}

func (m *Manager) reapLocked() {
	for id, cmd := range m.procs {
		if cmd == nil || cmd.Process == nil {
			delete(m.procs, id)
			continue
		}
		if cmd.ProcessState != nil && cmd.ProcessState.Exited() {
			delete(m.procs, id)
		}
	}
}

func (m *Manager) readMetadata(dir string) (Instance, error) {
	data, err := os.ReadFile(filepath.Join(dir, "instance.json"))
	if err != nil {
		return Instance{}, err
	}
	var item Instance
	if err := json.Unmarshal(data, &item); err != nil {
		return Instance{}, err
	}
	item.Dir = dir
	if item.ID == "" || item.Variant == "" {
		return Instance{}, fmt.Errorf("invalid metadata")
	}
	return item, nil
}

func writeMetadata(item Instance) error {
	data, err := json.Marshal(struct {
		ID      string        `json:"id"`
		Name    string        `json:"name"`
		Variant string        `json:"variant"`
		AdbPort int           `json:"adbPort"`
		Profile DeviceProfile `json:"profile"`
	}{item.ID, item.Name, item.Variant, item.AdbPort, item.Profile})
	if err != nil {
		return err
	}
	return os.WriteFile(filepath.Join(item.Dir, "instance.json"), append(data, '\n'), 0o644)
}

func (m *Manager) allocateAdbPort(skipID string) (int, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.allocateAdbPortLocked(skipID)
}

func (m *Manager) allocateAdbPortLocked(skipID string) (int, error) {
	used := map[int]bool{}
	entries, err := os.ReadDir(m.instancesDir())
	if err != nil && !os.IsNotExist(err) {
		return 0, err
	}
	for _, entry := range entries {
		if !entry.IsDir() || strings.HasPrefix(entry.Name(), ".") || entry.Name() == skipID {
			continue
		}
		item, err := m.readMetadata(filepath.Join(m.instancesDir(), entry.Name()))
		if err != nil || item.AdbPort <= 0 {
			continue
		}
		used[item.AdbPort] = true
	}

	h := fnv.New32a()
	_, _ = h.Write([]byte(skipID))
	base := 5555 + int(h.Sum32()%10000)
	for i := 0; i < 10000; i++ {
		port := 5555 + ((base - 5555 + i) % 10000)
		if used[port] {
			continue
		}
		if !portFree(port) {
			continue
		}
		return port, nil
	}
	return 0, fmt.Errorf("no free ADB port available")
}

func portFree(port int) bool {
	ln, err := net.Listen("tcp", fmt.Sprintf("127.0.0.1:%d", port))
	if err != nil {
		return false
	}
	_ = ln.Close()
	return true
}
