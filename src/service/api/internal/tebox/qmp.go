package tebox

import (
	"encoding/json"
	"fmt"
	"net"
	"strings"
	"time"
)

func qmpCommand(socketPath, command, key string) error {
	return qmpExec(socketPath, 500*time.Millisecond, commandJSON(command, key))
}

func commandJSON(command, key string) string {
	if key != "" {
		return fmt.Sprintf(
			`{"execute":"send-key","arguments":{"keys":[{"type":"qcode","data":%q}]}}`+"\n",
			key,
		)
	}
	return fmt.Sprintf(`{"execute":%q}`+"\n", command)
}

func qmpExec(socketPath string, timeout time.Duration, request string) error {
	conn, err := net.DialTimeout("unix", socketPath, 200*time.Millisecond)
	if err != nil {
		return err
	}
	defer conn.Close()
	_ = conn.SetDeadline(time.Now().Add(timeout))

	buf := make([]byte, 64<<10)
	_, _ = conn.Read(buf) // greeting
	if _, err := conn.Write([]byte("{\"execute\":\"qmp_capabilities\"}\n")); err != nil {
		return err
	}
	_, _ = conn.Read(buf)
	if _, err := conn.Write([]byte(request)); err != nil {
		return err
	}
	n, err := conn.Read(buf)
	if err != nil {
		return err
	}
	return qmpError(buf[:n])
}

func qmpError(raw []byte) error {
	for _, line := range strings.Split(string(raw), "\n") {
		line = strings.TrimSpace(line)
		if line == "" {
			continue
		}
		var msg struct {
			Error *struct {
				Class string `json:"class"`
				Desc  string `json:"desc"`
			} `json:"error"`
		}
		if json.Unmarshal([]byte(line), &msg) != nil || msg.Error == nil {
			continue
		}
		if msg.Error.Desc != "" {
			return fmt.Errorf("qmp: %s", msg.Error.Desc)
		}
		return fmt.Errorf("qmp: %s", msg.Error.Class)
	}
	return nil
}

func SendKey(socketPath, key string) error {
	return qmpCommand(socketPath, "", key)
}

func PowerDown(socketPath string) error {
	return qmpCommand(socketPath, "system_powerdown", "")
}

func SystemReset(socketPath string) error {
	return qmpCommand(socketPath, "system_reset", "")
}

func Screendump(socketPath, filename string) error {
	req := fmt.Sprintf(
		`{"execute":"screendump","arguments":{"filename":%q,"format":"ppm"}}`+"\n",
		filename,
	)
	return qmpExec(socketPath, 8*time.Second, req)
}
