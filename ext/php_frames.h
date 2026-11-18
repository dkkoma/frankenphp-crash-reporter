// The interface between the signal handler (handler.c), which includes no PHP
// header, and the PHP side of the extension (crash_reporter.c).

#ifndef CRASH_REPORTER_PHP_FRAMES_H
#define CRASH_REPORTER_PHP_FRAMES_H

#include <stddef.h>

#define MAX_PHP_FRAMES 32
// One more byte than the reporter prints, so that it knows to cut the front.
#define MAX_PHP_NAME 257

struct php_frame {
    char scope[MAX_PHP_NAME];
    size_t scope_len;
    char function[MAX_PHP_NAME];
    size_t function_len;
    char file[MAX_PHP_NAME];
    size_t file_len;
    unsigned long line;
};

struct php_frames {
    // Whether the crashed thread has PHP globals. Set before the first frame
    // is read.
    volatile int context;
    // Frames that were read completely. A fault while reading a frame leaves
    // the frames before it.
    volatile int count;
    volatile int omitted;
    struct php_frame frames[MAX_PHP_FRAMES];
};

#define HIDDEN __attribute__((visibility("hidden")))

// Reads the PHP call stack of the calling thread, innermost first. Called
// inside the signal handler, with a fault while reading caught by the caller.
HIDDEN void crash_reporter_collect_php_frames(struct php_frames *frames);

enum { ARMED, ERR_GO_MODULE_ENABLED, ERR_EXECUTABLE, ERR_GO_TEXT, ERR_GO_HANDLERS, ERR_INSTALL };

// Named apart from crash_reporter_arm of the Go module, which the executable
// may export.
HIDDEN int crash_reporter_ext_arm(void);
HIDDEN void crash_reporter_ext_disarm(void);

#endif
