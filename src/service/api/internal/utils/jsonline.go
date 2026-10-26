package utils

import (
	"encoding/json"
	"io"
	"sync"
)

// WriteJSONLine marshals v and writes one JSON line to w.
func WriteJSONLine(w io.Writer, v any) error {
	line, err := json.Marshal(v)
	if err != nil {
		return err
	}
	_, err = w.Write(append(line, '\n'))
	return err
}

// WriteJSONLineLocked is WriteJSONLine under mu.
func WriteJSONLineLocked(mu *sync.Mutex, w io.Writer, v any) error {
	mu.Lock()
	defer mu.Unlock()
	return WriteJSONLine(w, v)
}
