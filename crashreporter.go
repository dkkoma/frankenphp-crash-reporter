//go:build linux && cgo && (amd64 || arm64)

package crashreporter

/*
#include <stdint.h>

int crash_reporter_install(uintptr_t text_start, uintptr_t text_end);
uintptr_t crash_reporter_c_code_address(void);
*/
import "C"

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"reflect"
	"runtime"
)

func init() {
	if os.Getenv("FRANKENPHP_CRASH_REPORTER") != "true" {
		return
	}
	if err := install(); err != nil {
		logLine("WARNING", "crash reporter not armed: "+err.Error())
		return
	}
	logLine("INFO", "crash reporter armed")
}

func install() error {
	start, end := goTextRange()
	if err := checkGoTextRange(start, end); err != nil {
		return err
	}
	if C.crash_reporter_install(C.uintptr_t(start), C.uintptr_t(end)) != 0 {
		return errors.New("the Go signal handlers are not installed as expected")
	}
	return nil
}

func logLine(severity, message string) {
	line, _ := json.Marshal(struct {
		Severity string `json:"severity"`
		Message  string `json:"message"`
		Process  string `json:"process"`
	}{severity, message, filepath.Base(os.Args[0])})
	os.Stderr.Write(append(line, '\n'))
}

func isGoCode(pc uintptr) bool {
	return runtime.FuncForPC(pc) != nil
}

// The signal handler cannot ask the Go runtime whether a PC is Go code,
// so the range is resolved before the handler is installed.
func goTextRange() (start, end uintptr) {
	known := reflect.ValueOf(goTextRange).Pointer()
	return firstNonGoAddress(known, false) + 1, firstNonGoAddress(known, true)
}

func firstNonGoAddress(known uintptr, upward bool) uintptr {
	at := func(distance uintptr) uintptr {
		if upward {
			return known + distance
		}
		return known - distance
	}
	inside, outside := uintptr(0), uintptr(1)
	for isGoCode(at(outside)) {
		inside, outside = outside, outside*2
	}
	for outside-inside > 1 {
		middle := inside + (outside-inside)/2
		if isGoCode(at(middle)) {
			inside = middle
		} else {
			outside = middle
		}
	}
	return at(outside)
}

func checkGoTextRange(start, end uintptr) error {
	inRange := func(pc uintptr) bool { return pc >= start && pc < end }

	goCode := []uintptr{
		reflect.ValueOf(os.Getenv).Pointer(),
		reflect.ValueOf(runtime.GC).Pointer(),
		reflect.ValueOf(json.Marshal).Pointer(),
	}
	callers := make([]uintptr, 32)
	goCode = append(goCode, callers[:runtime.Callers(0, callers)]...)
	for _, pc := range goCode {
		if !inRange(pc) {
			return fmt.Errorf("Go code at %#x is outside the detected Go text range [%#x, %#x)", pc, start, end)
		}
	}
	if pc := uintptr(C.crash_reporter_c_code_address()); inRange(pc) {
		return fmt.Errorf("C code at %#x is inside the detected Go text range [%#x, %#x)", pc, start, end)
	}
	return nil
}
