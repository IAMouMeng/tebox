package utils

import (
	"fmt"
	"os"
	"syscall"
)

// CheckReadableImage requires a non-empty regular file the process can open.
func CheckReadableImage(path string) error {
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

// FileUsedMB is the host space a (sparse or cloned) file occupies, in MiB.
func FileUsedMB(path string) int {
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
