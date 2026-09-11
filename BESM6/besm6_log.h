/*
 * BESM-6 per-subsystem logging framework.
 *
 * Replaces the monolithic debug.txt approach with per-subsystem
 * log files, error collection, and automatic file rotation.
 *
 * Subsystem files:
 *   debug_sys.log   — system events, general messages
 *   debug_cpu.log   — CPU instructions, registers, operations
 *   debug_dks.log   — DKS/КАДОПАМ, terminal registration, PRP, interrupts
 *   debug_mmu.log   — MMU, page faults, cache
 *   debug_disk.log  — Disk operations
 *   debug_vu.log    — VU card reader
 *   debug_prn.log   — Printer (АЦПУ)
 *   debug_punch.log — Punch tape/card (ФС1500)
 *   debug_pl.log    — PL-80 printer
 *   debug_tty.log   — Terminal I/O (VT, TT, CONSUL, MUX)
 *   errors.log      — Errors from all subsystems
 *
 * Copyright (c) 1997-2009, Leonid Broukhis
 * Copyright (c) 2009, Serge Vakulenko
 * Modified 2026-09-11 — Modular logging framework
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 */

#ifndef BESM6_LOG_H
#define BESM6_LOG_H

#include <stdio.h>
#include <stdarg.h>

/*
 * Subsystem identifiers.
 */
typedef enum {
    B6_LOG_SYS = 0,   /* System / general */
    B6_LOG_CPU,        /* CPU instructions, registers */
    B6_LOG_DKS,        /* DKS/КАДОПАМ, terminal registration, PRP */
    B6_LOG_MMU,        /* MMU, page faults, cache */
    B6_LOG_DISK,       /* Disk operations */
    B6_LOG_VU,         /* VU card reader */
    B6_LOG_PRN,        /* Printer (АЦПУ) */
    B6_LOG_PUNCH,      /* Punch tape/card */
    B6_LOG_PL,         /* PL-80 printer */
    B6_LOG_TTY,        /* Terminal I/O (VT, TT, CONSUL, MUX) */
    B6_LOG_COUNT       /* Number of subsystems */
} b6_log_sub;

/*
 * Log levels.
 */
typedef enum {
    B6_LOG_ERROR = 0,
    B6_LOG_WARN,
    B6_LOG_INFO,
    B6_LOG_DEBUG,
    B6_LOG_TRACE
} b6_log_level;

/*
 * Initialize logging system.
 *   prefix: base directory for log files (NULL/"" = current dir)
 *   max_bytes: maximum file size per subsystem file (0 = no limit)
 * Returns 0 on success, -1 on error.
 */
int b6_log_init(const char *prefix, size_t max_bytes);

/*
 * Close all open log files.
 */
void b6_log_close(void);

/*
 * Log a message to a subsystem file.
 */
void b6_vlog(b6_log_sub subsystem, b6_log_level level, const char *fmt, va_list args);
void b6_log(b6_log_sub subsystem, b6_log_level level, const char *fmt, ...);

/*
 * Log an error (writes to subsystem file AND errors.log).
 */
void b6_log_error(b6_log_sub subsystem, const char *fmt, ...);

/*
 * Check file sizes and rotate if needed. Call periodically.
 */
void b6_log_rotate(void);

/*
 * Check if logging is initialized.
 */
int b6_log_is_init(void);

/*
 * Enable/disable subsystem logging (default: enabled).
 * When disabled, only sim_deb output (debug.txt) works.
 */
void b6_log_set_enabled(int enabled);
int  b6_log_is_enabled(void);

/*
 * Convenience macros for subsystem-specific logging.
 * These make it easy to switch call sites without changing all calls at once.
 *
 * Usage:
 *   BESM6_DKS_DEBUG(">>> DKS: terminal %d registered", num);
 *   BESM6_CPU_ERROR("UHV %o: address %o", addr);
 */
#define BESM6_SYS_DEBUG(fmt, ...)   b6_log(B6_LOG_SYS,   B6_LOG_DEBUG, fmt, ##__VA_ARGS__)
#define BESM6_CPU_DEBUG(fmt, ...)   b6_log(B6_LOG_CPU,   B6_LOG_DEBUG, fmt, ##__VA_ARGS__)
#define BESM6_DKS_DEBUG(fmt, ...)   b6_log(B6_LOG_DKS,   B6_LOG_DEBUG, fmt, ##__VA_ARGS__)
#define BESM6_MMU_DEBUG(fmt, ...)   b6_log(B6_LOG_MMU,   B6_LOG_DEBUG, fmt, ##__VA_ARGS__)
#define BESM6_DISK_DEBUG(fmt, ...)  b6_log(B6_LOG_DISK,  B6_LOG_DEBUG, fmt, ##__VA_ARGS__)
#define BESM6_VU_DEBUG(fmt, ...)    b6_log(B6_LOG_VU,    B6_LOG_DEBUG, fmt, ##__VA_ARGS__)
#define BESM6_PRN_DEBUG(fmt, ...)   b6_log(B6_LOG_PRN,   B6_LOG_DEBUG, fmt, ##__VA_ARGS__)
#define BESM6_PUNCH_DEBUG(fmt, ...) b6_log(B6_LOG_PUNCH,  B6_LOG_DEBUG, fmt, ##__VA_ARGS__)
#define BESM6_PL_DEBUG(fmt, ...)    b6_log(B6_LOG_PL,    B6_LOG_DEBUG, fmt, ##__VA_ARGS__)
#define BESM6_TTY_DEBUG(fmt, ...)   b6_log(B6_LOG_TTY,   B6_LOG_DEBUG, fmt, ##__VA_ARGS__)

/* Error macros */
#define BESM6_SYS_ERROR(fmt, ...)   b6_log_error(B6_LOG_SYS,   fmt, ##__VA_ARGS__)
#define BESM6_CPU_ERROR(fmt, ...)   b6_log_error(B6_LOG_CPU,   fmt, ##__VA_ARGS__)
#define BESM6_DKS_ERROR(fmt, ...)   b6_log_error(B6_LOG_DKS,   fmt, ##__VA_ARGS__)
#define BESM6_MMU_ERROR(fmt, ...)   b6_log_error(B6_LOG_MMU,   fmt, ##__VA_ARGS__)
#define BESM6_DISK_ERROR(fmt, ...)  b6_log_error(B6_LOG_DISK,  fmt, ##__VA_ARGS__)
#define BESM6_VU_ERROR(fmt, ...)    b6_log_error(B6_LOG_VU,    fmt, ##__VA_ARGS__)
#define BESM6_PRN_ERROR(fmt, ...)   b6_log_error(B6_LOG_PRN,   fmt, ##__VA_ARGS__)
#define BESM6_PUNCH_ERROR(fmt, ...) b6_log_error(B6_LOG_PUNCH, fmt, ##__VA_ARGS__)
#define BESM6_PL_ERROR(fmt, ...)    b6_log_error(B6_LOG_PL,    fmt, ##__VA_ARGS__)
#define BESM6_TTY_ERROR(fmt, ...)   b6_log_error(B6_LOG_TTY,   fmt, ##__VA_ARGS__)

/* Info-level macros */
#define BESM6_DKS_INFO(fmt, ...)    b6_log(B6_LOG_DKS,   B6_LOG_INFO,  fmt, ##__VA_ARGS__)
#define BESM6_CPU_INFO(fmt, ...)    b6_log(B6_LOG_CPU,   B6_LOG_INFO,  fmt, ##__VA_ARGS__)
#define BESM6_SYS_INFO(fmt, ...)    b6_log(B6_LOG_SYS,   B6_LOG_INFO,  fmt, ##__VA_ARGS__)

#endif /* BESM6_LOG_H */