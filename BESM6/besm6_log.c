/*
 * BESM-6 per-subsystem logging framework implementation.
 *
 * Creates separate log files per subsystem, with error collection
 * and automatic file rotation.
 *
 * Copyright (c) 1997-2009, Leonid Broukhis
 * Copyright (c) 2009, Serge Vakulenko
 * Modified 2026-09-11 — Modular logging framework
 */

#include "besm6_log.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>

/*
 * Subsystem names for file naming.
 */
static const char *subsys_names[] = {
    "sys", "cpu", "dks", "mmu", "disk", "vu", "prn", "punch", "pl", "tty"
};

/*
 * Level names for log output.
 */
static const char *level_names[] = {
    "ERROR", "WARN", "INFO", "DEBUG", "TRACE"
};

/*
 * File handles per subsystem.
 * NULL means the subsystem is not active.
 */
static FILE *sub_files[B6_LOG_COUNT];

/*
 * Error log file handle.
 * Errors are also written here (aggregated from all subsystems).
 */
static FILE *error_file;

/*
 * Maximum file size per subsystem (bytes). 0 = no limit.
 */
static size_t max_file_size;

/*
 * Base directory for log files.
 */
static char log_dir[256];

/*
 * Initialization flag.
 */
static int log_initialized;

/*
 * Enable/disable subsystem logging.
 * When disabled, only sim_deb output (debug.txt) continues to work.
 */
static int log_enabled = 1;

/*
 * Create a directory if it doesn't exist.
 * Returns 0 on success, -1 on error.
 */
static int
log_mkdir(const char *dir)
{
    struct stat st;
    if (stat(dir, &st) == 0) {
        return 0;  /* Already exists */
    }
    return mkdir(dir, 0755);
}

/*
 * Initialize logging system.
 * prefix: base directory for log files.
 *         If NULL or "", current directory is used.
 * max_bytes: maximum file size per subsystem file (0 = no limit).
 *
 * This function is called from besm6_init() or可以从 ini file command:
 *   set cpu log-dir=logs
 *   set cpu log-max=10
 *   set cpu log-init
 */
int
b6_log_init(const char *prefix, size_t max_bytes)
{
    int i;
    char path[512];

    if (log_initialized) {
        b6_log_close();
    }

    /* Ensure directory exists */
    log_dir[0] = '\0';
    if (prefix && prefix[0]) {
        log_mkdir(prefix);
        snprintf(log_dir, sizeof(log_dir), "%s/", prefix);
    }

    max_file_size = max_bytes;

    /* Clear file handles */
    for (i = 0; i < B6_LOG_COUNT; i++) {
        sub_files[i] = NULL;
    }
    error_file = NULL;

    /* Open per-subsystem files */
    for (i = 0; i < B6_LOG_COUNT; i++) {
        snprintf(path, sizeof(path), "%sdebug_%s.log", log_dir, subsys_names[i]);
        sub_files[i] = fopen(path, "a");
        if (!sub_files[i]) {
            /* Fallback: try without directory */
            snprintf(path, sizeof(path), "debug_%s.log", subsys_names[i]);
            sub_files[i] = fopen(path, "a");
        }
    }

    /* Open error log file */
    snprintf(path, sizeof(path), "%serrors.log", log_dir);
    error_file = fopen(path, "a");
    if (!error_file) {
        snprintf(path, sizeof(path), "errors.log");
        error_file = fopen(path, "a");
    }

    log_initialized = 1;
    return 0;
}

/*
 * Close all open log files.
 */
void
b6_log_close(void)
{
    int i;

    if (!log_initialized) {
        return;
    }

    for (i = 0; i < B6_LOG_COUNT; i++) {
        if (sub_files[i]) {
            fclose(sub_files[i]);
            sub_files[i] = NULL;
        }
    }
    if (error_file) {
        fclose(error_file);
        error_file = NULL;
    }

    log_initialized = 0;
}

/*
 * Update max file size for log rotation.
 * Can be called after b6_log_init() to change the limit.
 */
void
b6_log_set_max(size_t max_bytes)
{
    max_file_size = max_bytes;
}

/*
 * Check if the file at the given handle exceeds max_file_size.
 * If so, truncate it (simple rotation strategy).
 */
static void
log_check_rotate(FILE *f)
{
    long pos;
    if (!f || max_file_size == 0) {
        return;
    }
    pos = ftell(f);
    if (pos > 0 && (size_t)pos >= max_file_size) {
        /* Truncate the file */
        fseek(f, 0, SEEK_SET);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
        ftruncate(fileno(f), 0);
#pragma GCC diagnostic pop
        /* Write a marker that the file was rotated */
        fprintf(f, "[ROTATED at %ld bytes]\n", (long)max_file_size);
        fflush(f);
    }
}

/*
 * Log a message to a subsystem file.
 */
void
b6_vlog(b6_log_sub subsystem, b6_log_level level, const char *fmt, va_list args)
{
    FILE *f;

    if (!log_enabled || !log_initialized || subsystem < 0 || subsystem >= B6_LOG_COUNT) {
        return;
    }

    f = sub_files[subsystem];
    if (!f) {
        return;
    }

    /* Check rotation */
    log_check_rotate(f);

    /* Write the log message */
    fprintf(f, "[%s] ", level_names[level]);
    vfprintf(f, fmt, args);
    fprintf(f, "\n");
    fflush(f);
}

/*
 * Log a message (variadic wrapper).
 */
void
b6_log(b6_log_sub subsystem, b6_log_level level, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    b6_vlog(subsystem, level, fmt, args);
    va_end(args);
}

/*
 * Log an error (writes to subsystem file AND errors.log).
 */
void
b6_log_error(b6_log_sub subsystem, const char *fmt, ...)
{
    va_list args;

    if (!log_initialized) {
        return;
    }

    /* Write to subsystem file at ERROR level */
    va_start(args, fmt);
    b6_vlog(subsystem, B6_LOG_ERROR, fmt, args);
    va_end(args);

    /* Also write to errors.log */
    if (error_file) {
        log_check_rotate(error_file);

        va_start(args, fmt);
        fprintf(error_file, "[%s:%s] ", subsys_names[subsystem], "ERROR");
        vfprintf(error_file, fmt, args);
        fprintf(error_file, "\n");
        fflush(error_file);
        va_end(args);
    }
}

/*
 * Check all log file sizes and rotate if needed.
 * Call this periodically (e.g., from the timer service).
 */
void
b6_log_rotate(void)
{
    int i;

    if (!log_initialized || max_file_size == 0) {
        return;
    }

    for (i = 0; i < B6_LOG_COUNT; i++) {
        log_check_rotate(sub_files[i]);
    }
    if (error_file) {
        log_check_rotate(error_file);
    }
}

/*
 * Check if logging is initialized.
 */
int
b6_log_is_init(void)
{
    return log_initialized;
}

/*
 * Enable/disable subsystem logging.
 * When disabled, only sim_deb output (debug.txt) continues to work.
 * This allows disabling per-subsystem log files while keeping the
 * legacy debug output.
 */

void
b6_log_set_enabled(int enabled)
{
    log_enabled = enabled;
}

int
b6_log_is_enabled(void)
{
    return log_enabled;
}