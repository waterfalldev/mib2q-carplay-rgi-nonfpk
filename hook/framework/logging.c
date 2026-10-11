/*
 * CarPlay Hook Framework - Logging Implementation
 *
 * Copyright (c) 2026 LuKa (@LuKa_dev)
 */

#include "logging.h"

#if ENABLE_LOGGING || ENABLE_STATE_TRACE

#define MAX_LOG_LINE 1024
#define LOG_QUEUE_CAPACITY 64
#define LOG_SHUTDOWN_WAIT_MS 750

typedef struct {
    size_t len;
    char data[MAX_LOG_LINE];
} log_entry_t;

/* Module state */
static struct {
    int fd;
    log_config_t config;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool initialized;
    bool writer_created;
    bool writer_running;
    bool shutdown;
    pthread_t writer_tid;
    int owner_pid;
    log_entry_t queue[LOG_QUEUE_CAPACITY];
    unsigned queue_head;
    unsigned queue_tail;
    unsigned queue_count;
} g_log = {
    .fd = -1,
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER
};

/* Real write function pointer */
typedef ssize_t (*write_func_t)(int fd, const void* buf, size_t count);
static write_func_t real_write_fn = NULL;

static ssize_t do_write(int fd, const void* buf, size_t count) {
    if (!real_write_fn) {
        real_write_fn = (write_func_t)dlsym(RTLD_NEXT, "write");
    }
    if (real_write_fn) {
        return real_write_fn(fd, buf, count);
    }
    /* Fallback - should not happen */
    return -1;
}

static const char* level_str(log_level_t level) {
    switch (level) {
        case LOG_LEVEL_DEBUG: return "DBG";
        case LOG_LEVEL_INFO:  return "INF";
        case LOG_LEVEL_WARN:  return "WRN";
        case LOG_LEVEL_ERROR: return "ERR";
        default:              return "???";
    }
}

static void rotate_logs(void) {
    if (!g_log.config.log_path || g_log.config.max_files <= 0) return;

    char old_path[256], new_path[256];

    /* Remove oldest */
    snprintf(old_path, sizeof(old_path), "%s.%d",
             g_log.config.log_path, g_log.config.max_files);
    (void)unlink(old_path);

    /* Rotate existing */
    for (int i = g_log.config.max_files - 1; i >= 1; i--) {
        snprintf(old_path, sizeof(old_path), "%s.%d", g_log.config.log_path, i);
        snprintf(new_path, sizeof(new_path), "%s.%d", g_log.config.log_path, i + 1);
        (void)rename(old_path, new_path);
    }

    /* Current becomes .1 */
    snprintf(new_path, sizeof(new_path), "%s.1", g_log.config.log_path);
    (void)rename(g_log.config.log_path, new_path);
}

static void check_rotation(void) {
    if (g_log.fd < 0 || g_log.config.max_size == 0) return;

    /* Amortize: without this, fstat() is a syscall on EVERY log line.
     * Rotation isn't time-critical, so only probe the size every 64 writes
     * (this runs under g_log.lock, so the static counter is safe). */
    static unsigned rot_tick = 0;
    if ((++rot_tick & 63u) != 0) return;

    struct stat st;
    if (fstat(g_log.fd, &st) == 0 && (size_t)st.st_size >= g_log.config.max_size) {
        close(g_log.fd);
        g_log.fd = -1;
        rotate_logs();
        g_log.fd = open(g_log.config.log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    }
}

/* Only this thread performs normal log-file I/O.  Hooked stock threads format
 * one bounded record and enqueue it; a wedged /tmp filesystem can therefore
 * strand diagnostics, but can never strand dio_manager's RTSP/iAP2/watchdog
 * path.  The queue is fixed-size and drops the oldest record under pressure. */
static void* log_writer_main(void* unused) {
    (void)unused;
    for (;;) {
        log_entry_t entry;

        pthread_mutex_lock(&g_log.lock);
        while (!g_log.shutdown && g_log.queue_count == 0)
            pthread_cond_wait(&g_log.cond, &g_log.lock);
#if ENABLE_STATE_TRACE && !ENABLE_LOGGING
        /* Trace-only shutdown is itself evidence. Drain already-enqueued
         * terminal records before exiting; log_shutdown still caps the wait,
         * so a wedged filesystem cannot become a watchdog hang. */
        if (g_log.shutdown && g_log.queue_count == 0) {
#else
        if (g_log.shutdown) {
#endif
            g_log.writer_running = false;
            pthread_cond_broadcast(&g_log.cond);
            pthread_mutex_unlock(&g_log.lock);
            return NULL;
        }
        entry = g_log.queue[g_log.queue_head];
        g_log.queue_head = (g_log.queue_head + 1u) % LOG_QUEUE_CAPACITY;
        --g_log.queue_count;
        pthread_mutex_unlock(&g_log.lock);

        if (g_log.fd < 0 && g_log.config.log_path)
            g_log.fd = open(g_log.config.log_path,
                            O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (g_log.fd >= 0) {
            /* Unbuffered write, no fsync per line: that only stalls QNX /dev/shmem. */
            (void)do_write(g_log.fd, entry.data, entry.len);
            check_rotation();
        }
    }
}

/* Caller holds g_log.lock. Never fall back to synchronous writes: losing a
 * diagnostic is safer than putting a filesystem wait back on a stock thread. */
static bool log_start_writer_locked(void) {
    if (g_log.writer_created) return true;
    if (g_log.shutdown) return false;
    g_log.writer_running = true;
    if (pthread_create(&g_log.writer_tid, NULL, log_writer_main, NULL) != 0) {
        g_log.writer_running = false;
        return false;
    }
    g_log.writer_created = true;
    return true;
}

/* Production is WARN+ERROR.  One marker file lifts the whole hook to INFO for a diagnostic drive,
 * matching the Java side's /mnt/app/carplay_verbose.  MUST be shared by log_init() and the lazy
 * auto-init in log_write(): nothing actually calls log_init() (hook_framework.c keeps logging lazy on
 * purpose), so when this check lived only in log_init() the marker was dead code and INFO could never
 * be enabled at all — which is exactly how a session of audio probes came back empty. */
static void log_apply_defaults_locked(void) {
    log_config_t def = LOG_CONFIG_DEFAULT;
    g_log.config = def;
#if ENABLE_LOGGING
    if (log_verbose()) g_log.config.min_level = LOG_LEVEL_INFO;
#endif
}

hook_result_t log_init(const log_config_t* config) {
    pthread_mutex_lock(&g_log.lock);

    if (g_log.initialized) {
        pthread_mutex_unlock(&g_log.lock);
        return HOOK_OK;
    }

    /* Apply config or defaults */
    if (config) {
        g_log.config = *config;
    } else {
        log_apply_defaults_locked();
    }

    g_log.owner_pid = (int)getpid();
    g_log.shutdown = false;
    g_log.initialized = true;
    if (!log_start_writer_locked()) {
        g_log.initialized = false;
        pthread_mutex_unlock(&g_log.lock);
        return HOOK_ERR_INIT;
    }
    pthread_mutex_unlock(&g_log.lock);

    /* Write startup message */
    log_write(LOG_LEVEL_INFO, "LOG", "=== CarPlay Hook Log Started (pid=%d) ===", (int)getpid());

    return HOOK_OK;
}

void log_shutdown(void) {
    pthread_t writer;
    bool collect = false;

    pthread_mutex_lock(&g_log.lock);

    /* fork children inherit state/fds but not the writer thread. They must not
     * close or wait on the parent's logger during their ELF destruction. */
    if (g_log.owner_pid != 0 && g_log.owner_pid != (int)getpid()) {
        pthread_mutex_unlock(&g_log.lock);
        return;
    }

    g_log.shutdown = true;
#if !(ENABLE_STATE_TRACE && !ENABLE_LOGGING)
    g_log.queue_head = g_log.queue_tail = g_log.queue_count = 0;
#endif
    pthread_cond_broadcast(&g_log.cond);
    COND_WAIT_MS(&g_log.cond, &g_log.lock, LOG_SHUTDOWN_WAIT_MS,
                 !g_log.writer_created || !g_log.writer_running);
    if (g_log.writer_created && !g_log.writer_running) {
        writer = g_log.writer_tid;
        g_log.writer_created = false;
        collect = true;
    }
    if (g_log.writer_created) {
        /* The writer is stuck in file I/O. Preserve its fd/storage for process
         * exit and, critically, do not turn logger shutdown into a watchdog
         * wait. */
        pthread_mutex_unlock(&g_log.lock);
        return;
    }
    pthread_mutex_unlock(&g_log.lock);

    if (collect) pthread_join(writer, NULL); /* terminal-only collection */

    pthread_mutex_lock(&g_log.lock);

    if (g_log.fd >= 0) {
        close(g_log.fd);
        g_log.fd = -1;
    }

    g_log.initialized = false;
    g_log.owner_pid = 0;
    pthread_mutex_unlock(&g_log.lock);
}

void log_write(log_level_t level, const char* module, const char* fmt, ...) {
    log_level_t min_level;
    char line[MAX_LOG_LINE];
    int off = 0;

    if (g_log.initialized && level < g_log.config.min_level) return;
    pthread_mutex_lock(&g_log.lock);

    /* Auto-init if needed. Same defaults AND the same verbose-marker check as log_init() — this is the
     * path that actually runs, so the marker has to be honoured here or it does nothing. */
    if (!g_log.initialized) {
        log_apply_defaults_locked();
        g_log.owner_pid = (int)getpid();
        g_log.shutdown = false;
        g_log.initialized = true;
        if (!log_start_writer_locked()) g_log.initialized = false;
    }
    if (!g_log.initialized || g_log.shutdown ||
        (g_log.owner_pid != 0 && g_log.owner_pid != (int)getpid())) {
        pthread_mutex_unlock(&g_log.lock);
        return;
    }
    min_level = g_log.config.min_level;
    pthread_mutex_unlock(&g_log.lock);

    if (level < min_level) return;

    struct timespec ts;
    struct tm tm;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    off += snprintf(line + off, sizeof(line) - off, "%02d:%02d:%02d.%03d [%s] ",
                    tm.tm_hour, tm.tm_min, tm.tm_sec,
                    (int)(ts.tv_nsec / 1000000), level_str(level));
    if (module) off += snprintf(line + off, sizeof(line) - off, "[%s] ", module);

    /* snprintf reports the size it wanted, not the bytes it stored. Keep the
     * cursor inside the fixed stack record even if a future caller supplies an
     * unexpectedly long module name. */
    if (off < 0) off = 0;
    if (off >= (int)sizeof(line)) off = (int)sizeof(line) - 1;

    /* Message */
    va_list ap;
    va_start(ap, fmt);
    if (off < (int)sizeof(line)) {
        int available = (int)sizeof(line) - off;
        int written = vsnprintf(line + off, (size_t)available, fmt, ap);
        if (written > 0) {
            /* vsnprintf returns the untruncated size. Clamp before using off as
             * a memory length: passed to write(), the untruncated size would read
             * beyond this stack buffer. */
            off += (written >= available) ? (available - 1) : written;
        }
    }
    va_end(ap);

    /* Ensure newline */
    if (off > 0 && line[off - 1] != '\n' && off < (int)sizeof(line) - 1) {
        line[off++] = '\n';
    } else if (off == (int)sizeof(line) - 1 && line[off - 1] != '\n') {
        line[off - 1] = '\n';
    }
    if (off <= 0) return;

    pthread_mutex_lock(&g_log.lock);
    if (!g_log.initialized || g_log.shutdown || !g_log.writer_created ||
        g_log.owner_pid != (int)getpid()) {
        pthread_mutex_unlock(&g_log.lock);
        return;
    }
    if (g_log.queue_count == LOG_QUEUE_CAPACITY) {
        g_log.queue_head = (g_log.queue_head + 1u) % LOG_QUEUE_CAPACITY;
        --g_log.queue_count;
    }
    g_log.queue[g_log.queue_tail].len = (size_t)off;
    memcpy(g_log.queue[g_log.queue_tail].data, line, (size_t)off);
    g_log.queue_tail = (g_log.queue_tail + 1u) % LOG_QUEUE_CAPACITY;
    ++g_log.queue_count;
    pthread_cond_signal(&g_log.cond);
    pthread_mutex_unlock(&g_log.lock);
}

#if ENABLE_LOGGING

bool log_verbose(void) {
    static volatile int verbose = -1;       /* benign race: every caller stores the same value */
    if (verbose < 0)
        verbose = access("/mnt/app/carplay_verbose", F_OK) == 0
               || access("/tmp/carplay_verbose", F_OK) == 0;
    return verbose != 0;
}

#endif /* ENABLE_LOGGING */

#endif /* Logging or scoped trace writer. */
