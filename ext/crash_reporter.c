#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "SAPI.h"
#include "ext/standard/info.h"
#include "zend_globals.h"

#include <unistd.h>

#include "php_crash_reporter.h"
#include "php_frames.h"

static int arm_result = -1;

static void log_line(const char *severity, const char *message)
{
    char line[512];
    int n = snprintf(line, sizeof line, "{\"severity\":\"%s\",\"message\":\"%s\",\"source\":\"extension\"}\n",
                     severity, message);
    if (n > 0 && (size_t)n < sizeof line) {
        ssize_t written = write(STDERR_FILENO, line, (size_t)n);
        (void)written;
    }
}

static const char *not_armed_reason(int result)
{
    switch (result) {
    case ERR_GO_MODULE_ENABLED:
        return "crash reporter not armed: the Go module is enabled with FRANKENPHP_CRASH_REPORTER=true";
    case ERR_EXECUTABLE:
        return "crash reporter not armed: the executable could not be found";
    case ERR_GO_TEXT:
        return "crash reporter not armed: the code of the Go program could not be found";
    case ERR_GO_HANDLERS:
        return "crash reporter not armed: the Go signal handlers are not installed as expected";
    default:
        return "crash reporter not armed: the handlers could not be installed";
    }
}

PHP_MINIT_FUNCTION(crash_reporter)
{
    (void)type;
    (void)module_number;
    // The handler relies on the Go runtime of FrankenPHP; under any other SAPI
    // the extension does nothing.
    if (!sapi_module.name || strcmp(sapi_module.name, "frankenphp") != 0) {
        return SUCCESS;
    }
    arm_result = crash_reporter_ext_arm();
    if (arm_result == ARMED) {
        log_line("INFO", "crash reporter armed");
    } else {
        log_line("WARNING", not_armed_reason(arm_result));
    }
    return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(crash_reporter)
{
    (void)type;
    (void)module_number;
    if (arm_result == ARMED) {
        crash_reporter_ext_disarm();
    }
    arm_result = -1;
    return SUCCESS;
}

PHP_MINFO_FUNCTION(crash_reporter)
{
    (void)zend_module;
    php_info_print_table_start();
    php_info_print_table_row(2, "crash_reporter", arm_result == ARMED ? "armed" : "not armed");
    php_info_print_table_row(2, "Version", PHP_CRASH_REPORTER_VERSION);
    php_info_print_table_end();
}

zend_module_entry crash_reporter_module_entry = {
    STANDARD_MODULE_HEADER,
    "crash_reporter",
    NULL,
    PHP_MINIT(crash_reporter),
    PHP_MSHUTDOWN(crash_reporter),
    NULL,
    NULL,
    PHP_MINFO(crash_reporter),
    PHP_CRASH_REPORTER_VERSION,
    STANDARD_MODULE_PROPERTIES,
};

#ifdef COMPILE_DL_CRASH_REPORTER
ZEND_GET_MODULE(crash_reporter)
#endif

// Everything below runs inside the signal handler, on memory that may be
// corrupted. A fault while reading ends the walk; the frames read before it
// stay.

static zend_executor_globals *executor_globals_of_this_thread(void)
{
#ifdef ZTS
    // pthread_getspecific, which is not on the POSIX list of async-signal-safe
    // functions, but takes no lock and does not allocate in glibc.
    void *globals = tsrm_get_ls_cache();
    if (!globals) {
        return NULL;
    }
    return (zend_executor_globals *)((char *)globals + executor_globals_offset);
#else
    return &executor_globals;
#endif
}

static void copy_name(char *dst, size_t *dst_len, const zend_string *s)
{
    size_t len = ZSTR_LEN(s);
    size_t copied = len < MAX_PHP_NAME ? len : MAX_PHP_NAME;
    memcpy(dst, ZSTR_VAL(s) + (len - copied), copied);
    *dst_len = copied;
}

static unsigned long line_of(const zend_execute_data *ex, const zend_op_array *op_array)
{
    const zend_op *opline = ex->opline;
    if (opline && op_array->opcodes && opline >= op_array->opcodes && opline < op_array->opcodes + op_array->last) {
        return opline->lineno;
    }
    return op_array->line_start;
}

HIDDEN void crash_reporter_collect_php_frames(struct php_frames *frames)
{
    zend_executor_globals *eg = executor_globals_of_this_thread();
    if (!eg) {
        return;
    }
    frames->context = 1;
    const zend_execute_data *ex = eg->current_execute_data;
    for (int steps = 0; ex && steps < 10000; ex = ex->prev_execute_data, steps++) {
        const zend_function *func = ex->func;
        if (!func) {
            continue;
        }
        if (frames->count == MAX_PHP_FRAMES) {
            frames->omitted++;
            continue;
        }
        struct php_frame *frame = &frames->frames[frames->count];
        frame->scope_len = 0;
        frame->function_len = 0;
        frame->file_len = 0;
        frame->line = 0;
        if (func->common.function_name) {
            copy_name(frame->function, &frame->function_len, func->common.function_name);
            if (func->common.scope && func->common.scope->name) {
                copy_name(frame->scope, &frame->scope_len, func->common.scope->name);
            }
        }
        if (ZEND_USER_CODE(func->type) && func->op_array.filename) {
            copy_name(frame->file, &frame->file_len, func->op_array.filename);
            frame->line = line_of(ex, &func->op_array);
        }
        frames->count++;
    }
}
