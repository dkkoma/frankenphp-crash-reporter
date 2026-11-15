//go:build linux && cgo && (amd64 || arm64)

package crashreporter

/*
int crash_reporter_arm(void);
*/
import "C"

import (
	"encoding/json"
	"os"
)

func init() {
	if os.Getenv("FRANKENPHP_CRASH_REPORTER") != "true" {
		return
	}
	switch C.crash_reporter_arm() {
	case 0:
		logLine("INFO", "crash reporter armed")
	case 1:
		logLine("WARNING", "crash reporter not armed: the handlers could not be installed before the Go runtime started")
	default:
		logLine("WARNING", "crash reporter not armed: the Go signal handlers are not installed as expected")
	}
}

func logLine(severity, message string) {
	line, _ := json.Marshal(struct {
		Severity string `json:"severity"`
		Message  string `json:"message"`
	}{severity, message})
	os.Stderr.Write(append(line, '\n'))
}
