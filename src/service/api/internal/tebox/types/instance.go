package types

// Instance is one Android guest managed by tebox-api.
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
	// ControlPort is always 0 (shared TEBOX-CTRL / UDP endpoint only).
	ControlPort int `json:"controlPort"`
	// Resources fixed at creation; 0 in old metadata means the boot defaults.
	CPUCores int `json:"cpuCores"`
	MemoryMB int `json:"memoryMb"`
	DiskMB   int `json:"diskMb"`
	// DiskUsedMB is the space userdata.img occupies on the host.
	DiskUsedMB int `json:"diskUsedMb"`
	// State is stopped, starting, running, or stopping.
	State string `json:"state"`
	// Images set at creation; empty means the variant's own image.
	SystemImage    string `json:"systemImage,omitempty"`
	VendorImage    string `json:"vendorImage,omitempty"`
	InitramfsImage string `json:"initramfsImage,omitempty"`
	// RunDir is the instance directory chosen at creation (empty: default under data root).
	RunDir string `json:"runDir,omitempty"`

	// MetaDir holds instance.json under the data root.
	MetaDir string `json:"-"`
}

// CreateOptions sizes a new instance; zero fields take the defaults.
type CreateOptions struct {
	Name     string
	CPUCores int
	MemoryMB int
	DiskMB   int
	SystemImage    string
	VendorImage    string
	InitramfsImage string
	InstanceDir    string
}

// DeviceProfile is guest identity / location applied over adb.
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
