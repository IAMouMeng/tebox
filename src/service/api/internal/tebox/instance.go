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
	// CastPort is the per-instance tebox-cast port while casting (0 otherwise).
	CastPort int `json:"castPort"`
	// ControlPort is the per-instance input port while running (0 otherwise).
	ControlPort int `json:"controlPort"`
	// Resources fixed at creation; 0 in old metadata means the boot defaults.
	CPUCores int `json:"cpuCores"`
	MemoryMB int `json:"memoryMb"`
	DiskMB   int `json:"diskMb"`
	// DiskUsedMB is the space userdata.img occupies on the host.
	DiskUsedMB int `json:"diskUsedMb"`
	// State is stopped, starting (QEMU up, Android booting), running
	// (sys.boot_completed) or stopping.
	State string `json:"state"`
	// Images set at creation; empty means the variant's own image.
	SystemImage    string `json:"systemImage,omitempty"`
	VendorImage    string `json:"vendorImage,omitempty"`
	InitramfsImage string `json:"initramfsImage,omitempty"`
	// RunDir is the instance directory chosen at creation (empty: the
	// default under the data root, which always holds instance.json).
	RunDir string `json:"runDir,omitempty"`

	metaDir string // data root entry holding instance.json
}

// CreateOptions sizes a new instance; zero fields take the defaults.
type CreateOptions struct {
	Name     string
	CPUCores int
	MemoryMB int
	DiskMB   int
	// Absolute image paths; empty takes the variant's image.
	SystemImage    string
	VendorImage    string
	InitramfsImage string
	// InstanceDir holds userdata.img and sockets; empty takes
	// <data root>/instances/<id>. It must be absent or empty, and Delete
	// removes it.
	InstanceDir string
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

	mu        sync.Mutex
	procs     map[string]*exec.Cmd
	castProcs map[string]*exec.Cmd
	castPorts map[string]int
	castFails map[string]int // consecutive short-lived tebox-cast runs
	input     inputHub
	booted    map[string]bool // sys.boot_completed seen for the running process
	stopping  map[string]bool
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
		castProcs:   make(map[string]*exec.Cmd),
		castPorts:   make(map[string]int),
		castFails:   make(map[string]int),
		booted:      make(map[string]bool),
		stopping:    make(map[string]bool),
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
		item.State = StateStopped
		if cmd := m.procs[item.ID]; cmd != nil && cmd.Process != nil {
			item.PID = cmd.Process.Pid
			item.Running = true
			switch {
			case m.stopping[item.ID]:
				item.State = StateStopping
			case m.booted[item.ID]:
				item.State = StateRunning
			default:
				item.State = StateStarting
			}
		}
		item.CastPort = m.castPorts[item.ID]
		item.ControlPort = m.input.port(item.ID)
		out = append(out, m.withImages(item))
	}
	return out, nil
}

func (m *Manager) Create(opts CreateOptions) (Instance, error) {
	if err := opts.normalize(); err != nil {
		return Instance{}, err
	}
	now := time.Now()
	item := Instance{
		ID:       fmt.Sprintf("android-%d-%d", now.Unix(), now.Nanosecond()/1e6),
		Name:     fmt.Sprintf("Android %d", now.Unix()),
		Variant:  m.Variant,
		Profile:  DeviceProfile{Latitude: 39.9042, Longitude: 116.4074, Country: "CN", OperatorNumeric: "46000", OperatorName: "China Mobile", CellId: "0x1001", PhoneNumber: "+8613800138000"},
		CPUCores: opts.CPUCores,
		MemoryMB: opts.MemoryMB,
		DiskMB:   opts.DiskMB,
		State:    StateStopped,

		SystemImage:    opts.SystemImage,
		VendorImage:    opts.VendorImage,
		InitramfsImage: opts.InitramfsImage,
		RunDir:         opts.InstanceDir,
	}
	if opts.Name != "" {
		item.Name = opts.Name
	}
	item.metaDir = filepath.Join(m.instancesDir(), item.ID)
	item.Dir = item.metaDir
	if item.RunDir != "" {
		item.Dir = item.RunDir
	}
	port, err := m.allocateAdbPort(item.ID)
	if err != nil {
		return Instance{}, err
	}
	item.AdbPort = port
	if err := os.MkdirAll(item.metaDir, 0o755); err != nil {
		return Instance{}, err
	}
	if err := os.MkdirAll(item.Dir, 0o755); err != nil {
		_ = os.RemoveAll(item.metaDir)
		return Instance{}, err
	}
	if item.RunDir != "" {
		// Claims the directory: it is no longer empty for another
		// CreateInstance, and Delete checks it before removing anything.
		if err := os.WriteFile(filepath.Join(item.Dir, runDirMarker), []byte(item.ID+"\n"), 0o644); err != nil {
			_ = os.RemoveAll(item.metaDir)
			return Instance{}, err
		}
	}
	if err := writeMetadata(item); err != nil {
		return Instance{}, err
	}
	return m.withImages(item), nil
}

// runDirMarker names the owning instance inside a chosen instance_dir.
const runDirMarker = ".tebox-instance"

// withImages fills the image paths the instance boots, defaulting to the
// variant's images.
func (m *Manager) withImages(item Instance) Instance {
	images := filepath.Join(m.ProjectRoot, "src", "aosp", item.Variant, "images")
	if item.SystemImage == "" {
		item.SystemImage = filepath.Join(images, "system.img")
	}
	if item.VendorImage == "" {
		item.VendorImage = filepath.Join(images, "vendor.img")
	}
	if item.InitramfsImage == "" {
		item.InitramfsImage = filepath.Join(images, "initramfs.img")
	}
	return item
}

// imageEnv passes the instance's own images to ./run, after checking they
// are still there.
func imageEnv(item Instance) ([]string, error) {
	var env []string
	for _, img := range []struct{ key, path string }{
		{"SYSTEM_IMG", item.SystemImage},
		{"VENDOR_IMG", item.VendorImage},
		{"INITRD_IMG", item.InitramfsImage},
	} {
		if img.path == "" {
			continue
		}
		if err := checkImage(img.path); err != nil {
			return nil, err
		}
		env = append(env, img.key+"="+img.path)
	}
	return env, nil
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
	if cast := m.castProcs[id]; cast != nil && cast.Process != nil {
		_ = cast.Process.Kill()
		delete(m.castProcs, id)
		delete(m.castPorts, id)
	}
	m.input.stop(id)
	if item.Dir != item.metaDir {
		owner, _ := os.ReadFile(filepath.Join(item.Dir, runDirMarker))
		if strings.TrimSpace(string(owner)) != item.ID {
			return fmt.Errorf("instance %s: %s is not marked as its directory; not removing it", id, item.Dir)
		}
		if err := os.RemoveAll(item.Dir); err != nil {
			return err
		}
	}
	return os.RemoveAll(item.metaDir)
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
	images, err := imageEnv(item)
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
	_ = os.Remove(inputQMPPath(item.Dir))

	cmd := exec.Command(runPath, item.Variant)
	cmd.Dir = m.ProjectRoot
	cmd.Env = append(os.Environ(),
		"TEBOX_INSTANCE_DIR="+item.Dir,
		"QMP_SOCKET="+qmpPath,
		"QMP_INPUT_SOCKET="+inputQMPPath(item.Dir),
		"SNAPSHOT=0",
		"FORCE_VIRGL=1",
		"QEMU_DEBUG=1",
		fmt.Sprintf("ADB_PORT=%d", item.AdbPort),
		"QEMU_NAME="+item.Name,
		fmt.Sprintf("SMP=%d", item.CPUCores),
		fmt.Sprintf("MEM=%d", item.MemoryMB),
		fmt.Sprintf("USERDATA_MB=%d", item.DiskMB),
		"TEBOX_CAST=1",
		"TEBOX_CAST_SOCK="+filepath.Join(item.Dir, "cast.sock"),
	)
	cmd.Env = append(cmd.Env, images...)
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	delete(m.castFails, id)
	m.startCastLocked(item)
	if err := cmd.Start(); err != nil {
		if cast := m.castProcs[item.ID]; cast != nil && cast.Process != nil {
			_ = cast.Process.Kill()
			delete(m.castProcs, item.ID)
			delete(m.castPorts, item.ID)
		}
		return err
	}
	m.procs[id] = cmd
	m.input.start(id, inputQMPPath(item.Dir), m.ProjectRoot, fmt.Sprintf("127.0.0.1:%d", item.AdbPort))
	delete(m.booted, id)
	delete(m.stopping, id)
	go m.watchBoot(id, cmd, fmt.Sprintf("127.0.0.1:%d", item.AdbPort))
	go func() {
		_ = cmd.Wait()
		m.mu.Lock()
		if m.procs[id] == cmd {
			delete(m.procs, id)
			delete(m.booted, id)
			delete(m.stopping, id)
			m.input.stop(id)
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
	if cast := m.castProcs[id]; cast != nil && cast.Process != nil {
		_ = cast.Process.Kill()
		delete(m.castProcs, id)
		delete(m.castPorts, id)
	}
	if cmd == nil || cmd.Process == nil {
		return nil
	}
	m.stopping[id] = true
	if path, err := m.QMPPath(id); err == nil {
		_ = PowerDown(path)
	}
	_ = cmd.Process.Signal(os.Interrupt)
	return nil
}

func (m *Manager) startCastLocked(item Instance) {
	if _, ok := m.castProcs[item.ID]; ok {
		return
	}
	binary := filepath.Join(m.ProjectRoot, "out", "service", "tebox-cast")
	if _, err := os.Stat(binary); err != nil {
		return
	}
	port, err := allocatePort(12000, m.castPorts)
	if err != nil {
		return
	}
	sock := filepath.Join(item.Dir, "cast.sock")
	_ = os.Remove(sock)
	// Control port 0: every instance runs its own tebox-cast, and a fixed
	// default (9902) would make the second one fail to start.
	cmd := exec.Command(binary, "--capture-sock", sock, "--listen", fmt.Sprintf("127.0.0.1:%d", port),
		"--control", "127.0.0.1:0", "--fps", "60", "--bitrate", "8000000")
	cmd.Stdout, cmd.Stderr = os.Stdout, os.Stderr
	if err := cmd.Start(); err != nil {
		return
	}
	m.castProcs[item.ID], m.castPorts[item.ID] = cmd, port
	started := time.Now()
	go func() {
		err := cmd.Wait()
		m.mu.Lock()
		defer m.mu.Unlock()
		if m.castProcs[item.ID] != cmd {
			return // killed by Stop/Delete
		}
		delete(m.castProcs, item.ID)
		delete(m.castPorts, item.ID)
		if m.procs[item.ID] == nil || m.stopping[item.ID] {
			return
		}
		// The guest keeps running, so bring the cast back; QEMU's capture
		// hook reconnects to cast.sock on its own. Back off on crash loops.
		if time.Since(started) > time.Minute {
			m.castFails[item.ID] = 0
		}
		delay := time.Second << min(m.castFails[item.ID], 5)
		m.castFails[item.ID]++
		fmt.Fprintf(os.Stderr, "tebox-api: tebox-cast for %s exited (%v), restarting in %s\n", item.ID, err, delay)
		time.AfterFunc(delay, func() {
			m.mu.Lock()
			defer m.mu.Unlock()
			if m.procs[item.ID] != nil && !m.stopping[item.ID] {
				m.startCastLocked(item)
			}
		})
	}()
}

func (m *Manager) CastPort(id string) (int, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	if port := m.castPorts[id]; port > 0 {
		return port, nil
	}
	return 0, fmt.Errorf("cast is not running for instance %s", id)
}

// Reset reboots a running guest through QMP. With wipeData, it recreates the
// instance userdata image on the next boot, which provides a factory reset.
func (m *Manager) Reset(id string, wipeData bool) error {
	if wipeData {
		if _, err := m.stopAndWait(id, "factory reset"); err != nil {
			return err
		}
		item, err := m.readMetadata(filepath.Join(m.instancesDir(), id))
		if err != nil {
			return err
		}
		if err := os.Remove(filepath.Join(item.Dir, "userdata.img")); err != nil && !os.IsNotExist(err) {
			return fmt.Errorf("remove userdata: %w", err)
		}
		return m.Start(id)
	}

	path, err := m.QMPPath(id)
	if err != nil {
		return err
	}
	if err := SystemReset(path); err != nil {
		return err
	}
	// The guest reboots: back to "starting" until Android boots again.
	m.mu.Lock()
	if cmd := m.procs[id]; cmd != nil {
		delete(m.booted, id)
		if item, rerr := m.readMetadata(filepath.Join(m.instancesDir(), id)); rerr == nil {
			go m.watchBoot(id, cmd, fmt.Sprintf("127.0.0.1:%d", item.AdbPort))
		}
	}
	m.mu.Unlock()
	return nil
}

// stopAndWait stops a running instance and waits for QEMU to exit; it
// reports whether the instance was running.
func (m *Manager) stopAndWait(id, why string) (bool, error) {
	m.mu.Lock()
	cmd := m.procs[id]
	m.mu.Unlock()
	if cmd == nil || cmd.Process == nil {
		return false, nil
	}
	if err := m.Stop(id); err != nil {
		return true, err
	}
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		m.mu.Lock()
		stopped := m.procs[id] == nil
		m.mu.Unlock()
		if stopped {
			return true, nil
		}
		time.Sleep(50 * time.Millisecond)
	}
	return true, fmt.Errorf("instance %s did not stop before %s", id, why)
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

// InputQMPPath is the QEMU monitor reserved for streamed cast input.
func (m *Manager) InputQMPPath(id string) (string, error) {
	item, err := m.readMetadata(filepath.Join(m.instancesDir(), id))
	if err != nil {
		return "", err
	}
	return inputQMPPath(item.Dir), nil
}

func inputQMPPath(dir string) string { return filepath.Join(dir, "qmp-input.sock") }

func (m *Manager) ShutdownRunning() {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.input.stopAll()
	for id, cmd := range m.procs {
		if cmd == nil || cmd.Process == nil {
			continue
		}
		if path, err := m.QMPPath(id); err == nil {
			_ = PowerDown(path)
		}
	}
	for id, cmd := range m.castProcs {
		if cmd != nil && cmd.Process != nil {
			_ = cmd.Process.Kill()
			delete(m.castProcs, id)
		}
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
	item.metaDir, item.Dir = dir, dir
	if item.RunDir != "" {
		item.Dir = item.RunDir
	}
	if item.ID == "" || item.Variant == "" {
		return Instance{}, fmt.Errorf("invalid metadata")
	}
	item.fillResources()
	return item, nil
}

func writeMetadata(item Instance) error {
	data, err := json.Marshal(struct {
		ID       string        `json:"id"`
		Name     string        `json:"name"`
		Variant  string        `json:"variant"`
		AdbPort  int           `json:"adbPort"`
		Profile  DeviceProfile `json:"profile"`
		CPUCores int           `json:"cpuCores"`
		MemoryMB int           `json:"memoryMb"`
		DiskMB   int           `json:"diskMb"`

		SystemImage    string `json:"systemImage,omitempty"`
		VendorImage    string `json:"vendorImage,omitempty"`
		InitramfsImage string `json:"initramfsImage,omitempty"`
		RunDir         string `json:"runDir,omitempty"`
	}{item.ID, item.Name, item.Variant, item.AdbPort, item.Profile, item.CPUCores, item.MemoryMB, item.DiskMB,
		item.SystemImage, item.VendorImage, item.InitramfsImage, item.RunDir})
	if err != nil {
		return err
	}
	return os.WriteFile(filepath.Join(item.metaDir, "instance.json"), append(data, '\n'), 0o644)
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

func allocatePort(start int, used map[string]int) (int, error) {
	for port := start; port < 65535; port++ {
		busy := false
		for _, value := range used {
			if value == port {
				busy = true
				break
			}
		}
		if !busy && portFree(port) {
			return port, nil
		}
	}
	return 0, fmt.Errorf("no free cast port available")
}
