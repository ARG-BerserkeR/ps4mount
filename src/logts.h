/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Log file with the time at the start of every line, kept under 10 MB: past that it is cut
 * back to its last 256 KB, at start and while running. */
#pragma once

#include <stddef.h>

/* Sends stdout to path (appended), each line prefixed with the date and time. 0 on
 * success; on failure stdout is left as it was. */
int log_start(const char *path);
/* Writes what is still pending and waits for it; safe to call more than once. */
void log_stop(void);

/* "2026-10-14 13:52:06.123 " for the current time (local time of the console). */
void log_stamp(char *out, size_t n);
/* Writes buf to fd, putting stamp before every line; *at_start tracks whether the next
 * byte begins a line (start with 1). Returns 0, or -1 when a write failed. */
int log_write_lines(int fd, int *at_start, const char *buf, size_t len, const char *stamp);
/* When path is larger than max bytes, keeps only its last keep bytes (from the first whole
 * line). Returns 1 when cut, 0 when left alone, -1 on error. */
int log_shrink(const char *path, long max, long keep);
