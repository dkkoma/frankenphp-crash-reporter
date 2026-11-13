// Package crashreporter makes FrankenPHP print one JSON line to stderr when
// the process is about to die from a fatal signal raised outside Go code.
//
// Link it with a blank import (xcaddy build --with) and set
// FRANKENPHP_CRASH_REPORTER=true to enable it.
package crashreporter
