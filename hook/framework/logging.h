/*
 * CarPlay Hook Framework - Logging API
 *
 * Unified logging solution with:
 * - Multiple log levels (DEBUG, INFO, WARN, ERROR)
 * - Per-module tagging
 * - File output with rotation support
 * - Thread-safe operation
 *
 * Define ENABLE_LOGGING=1 to enable logging, or ENABLE_LOGGING=0 to disable.
 * When disabled, all logging macros become no-ops with zero overhead.
 *
 * Copyright (c) 2026 LuKa (@LuKa_dev)
 */

#ifndef CARPLAY_LOGGING_H
#define CARPLAY_LOGGING_H

#include "common.h"

/* Global logging switch - define ENABLE_LOGGING=0 to disable all logging */
#ifndef ENABLE_LOGGING
#define ENABLE_LOGGING 1
#endif

/* Log levels */
typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO = 1,
    LOG_LEVEL_WARN = 2,
    LOG_LEVEL_ERROR = 3
} log_level_t;

/* Log configuration */
typedef struct {
    const char* log_path;       /* Path to log file (NULL = stdout) */
    log_level_t min_level;      /* Minimum level to log */
    size_t max_size;            /* Max file size before rotation (0 = no limit) */
    int max_files;              /* Max number of rotated files to keep */
} log_config_t;

/* Default configuration.
 * Production emits WARN and ERROR only.  INFO/DEBUG stay compiled in and are lifted to INFO
 * without a rebuild by `touch /mnt/app/carplay_verbose` - see log_init() in logging.c. */
#define LOG_DEFAULT_PATH "/tmp/carplay_hook.log"

#define LOG_CONFIG_DEFAULT { \
    .log_path = LOG_DEFAULT_PATH, \
    .min_level = LOG_LEVEL_WARN, \
    .max_size = 1024 * 1024, \
    .max_files = 3 \
}

#if ENABLE_LOGGING

/* Initialize logging system */
hook_result_t log_init(const log_config_t* config);

/* Shutdown logging system */
void log_shutdown(void);

/* Core logging function */
void log_write(log_level_t level, const char* module, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));

#else /* All logging and scoped traces disabled. */

/* No-op versions when logging is disabled */
static inline hook_result_t log_init(const log_config_t* config) {
    (void)config; return HOOK_OK;
}
static inline void log_shutdown(void) {}
static inline void log_write(log_level_t level, const char* module, const char* fmt, ...) {
    (void)level; (void)module; (void)fmt;
}

#endif /* Logging or scoped trace writer. */

#if ENABLE_LOGGING

/* Convenience macros for logging */
#define LOG_INFO(module, fmt, ...) \
    log_write(LOG_LEVEL_INFO, module, fmt, ##__VA_ARGS__)

#define LOG_WARN(module, fmt, ...) \
    log_write(LOG_LEVEL_WARN, module, fmt, ##__VA_ARGS__)

#define LOG_ERROR(module, fmt, ...) \
    log_write(LOG_LEVEL_ERROR, module, fmt, ##__VA_ARGS__)

/* Module-specific logging helpers */
#define DEFINE_LOG_MODULE(name) \
    static const char* LOG_MODULE = #name

/* True when /mnt/app/carplay_verbose or /tmp/carplay_verbose existed at the first call
 * (checked once per process, i.e. per dio_manager session).  It lifts the log to INFO.
 * Without it the hook logs WARN/ERROR only. */
bool log_verbose(void);

#else /* ENABLE_LOGGING == 0 */

/* No-op versions when logging is disabled */
static inline bool log_verbose(void) { return false; }
#define LOG_INFO(module, fmt, ...)       ((void)0)
#define LOG_WARN(module, fmt, ...)       ((void)0)
#define LOG_ERROR(module, fmt, ...)      ((void)0)
#define DEFINE_LOG_MODULE(name)          /* empty */

#endif /* ENABLE_LOGGING */

#endif /* CARPLAY_LOGGING_H */
