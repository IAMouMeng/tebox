package utils

import (
	"fmt"
	"regexp"
	"unicode"

	"tebox-api/internal/constants"
)

var qcodePattern = regexp.MustCompile(`^[a-z0-9_]{1,24}$`)

// QCodesForAndroidKey maps an Android keycode (+ meta) to QEMU qcodes.
func QCodesForAndroidKey(keycode, meta int) ([]string, error) {
	base, ok := constants.AndroidKeyToQCode[keycode]
	if !ok {
		return nil, fmt.Errorf("unsupported android keycode %d (QEMU virtio path)", keycode)
	}
	var mods []string
	if meta&constants.AndroidMetaCtrl != 0 {
		mods = append(mods, "ctrl")
	}
	if meta&constants.AndroidMetaShift != 0 {
		mods = append(mods, "shift")
	}
	out := make([]string, 0, len(mods)+1)
	out = append(out, mods...)
	out = append(out, base)
	return out, nil
}

// QNumForAndroidKey returns a QMP key number when there is no QKeyCode.
func QNumForAndroidKey(keycode int) (int, bool) {
	n, ok := constants.AndroidKeyToQNum[keycode]
	return n, ok
}

// QCodesForText maps printable ASCII to qcode sequences (shift when needed).
func QCodesForText(text string) ([][]string, error) {
	var seqs [][]string
	for _, r := range text {
		codes, err := QCodesForRune(r)
		if err != nil {
			return nil, err
		}
		seqs = append(seqs, codes)
	}
	return seqs, nil
}

// QCodesForRune maps one rune to a qcode press sequence.
func QCodesForRune(r rune) ([]string, error) {
	switch r {
	case '\n', '\r':
		return []string{"ret"}, nil
	case '\t':
		return []string{"tab"}, nil
	case ' ':
		return []string{"spc"}, nil
	}
	if q, ok := constants.ShiftedRunes[r]; ok {
		return []string{"shift", q}, nil
	}
	if unicode.IsUpper(r) {
		return []string{"shift", string(unicode.ToLower(r))}, nil
	}
	if (r >= 'a' && r <= 'z') || (r >= '0' && r <= '9') {
		return []string{string(r)}, nil
	}
	if q, ok := constants.PlainRunes[r]; ok {
		return []string{q}, nil
	}
	return nil, fmt.Errorf("non-ASCII text %q needs a guest IME; use QEMU window-style ASCII", string(r))
}

// ResolveNavKey returns a QMP key value for a friendly or raw qcode name.
func ResolveNavKey(name string) (qcode string, qnum int, useNum bool, err error) {
	if n, ok := constants.NavKeyToQNum[name]; ok {
		return "", n, true, nil
	}
	if code := constants.NavKeyToQCode[name]; code != "" {
		return code, 0, false, nil
	}
	if qcodePattern.MatchString(name) {
		return name, 0, false, nil
	}
	return "", 0, false, fmt.Errorf("unknown key %q", name)
}
