#ifndef PHP_CRASH_REPORTER_H
#define PHP_CRASH_REPORTER_H

extern zend_module_entry crash_reporter_module_entry;
#define phpext_crash_reporter_ptr &crash_reporter_module_entry

#define PHP_CRASH_REPORTER_VERSION "0.1.0"

#endif
