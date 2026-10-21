/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Log file with the time at the start of every line: stdout goes into a pipe, and a thread
 * copies it to the log file, adding the time before each line. Every printf of the payload
 * gets its time this way. The file is kept under LOG_MAX bytes. */
#include "logts.h"

#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* past LOG_MAX bytes, only the last LOG_KEEP bytes of the log are kept */
#define LOG_MAX (10 << 20)
#define LOG_KEEP (256 << 10)

static int write_all(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w <= 0)
            return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

void log_stamp(char *out, size_t n) {
    struct timespec ts;
    struct tm tm;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    char day[32];
    strftime(day, sizeof(day), "%Y-%m-%d %H:%M:%S", &tm);
    snprintf(out, n, "%s.%03ld ", day, ts.tv_nsec / 1000000);
}

int log_write_lines(int fd, int *at_start, const char *buf, size_t len, const char *stamp) {
    size_t sl = strlen(stamp);
    while (len) {
        if (*at_start && write_all(fd, stamp, sl))
            return -1;
        const char *nl = memchr(buf, '\n', len);
        size_t take = nl ? (size_t)(nl - buf) + 1 : len;
        if (write_all(fd, buf, take))
            return -1;
        *at_start = nl != NULL;
        buf += take;
        len -= take;
    }
    return 0;
}

int log_shrink(const char *path, long max, long keep) {
    struct stat st;
    int in = open(path, O_RDONLY);
    if (in < 0)
        return 0;
    if (fstat(in, &st) || st.st_size <= max) {
        close(in);
        return 0;
    }
    if (keep > st.st_size)
        keep = st.st_size;
    char *buf = malloc((size_t)keep);
    ssize_t got = -1;
    if (buf && lseek(in, st.st_size - keep, SEEK_SET) >= 0)
        got = read(in, buf, (size_t)keep);
    close(in);
    if (got < 0) {
        free(buf);
        return -1;
    }
    /* the kept part starts on a whole line */
    const char *nl = memchr(buf, '\n', (size_t)got);
    size_t skip = nl ? (size_t)(nl - buf) + 1 : (size_t)got;
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    int out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    int rc = out < 0 || write_all(out, buf + skip, (size_t)got - skip) ? -1 : 0;
    if (out >= 0)
        close(out);
    free(buf);
    if (!rc && rename(tmp, path))
        rc = -1;
    if (rc)
        unlink(tmp);
    return rc ? -1 : 1;
}

static int g_pipe_read = -1, g_file = -1;
static char g_path[PATH_MAX];
static pthread_t g_thread;
static int g_running;

/* Run by the copy thread, the only writer of the file, between whole lines. */
static void keep_small(void) {
    struct stat st;
    if (fstat(g_file, &st) || st.st_size <= LOG_MAX || log_shrink(g_path, LOG_MAX, LOG_KEEP) != 1)
        return;
    int fd = open(g_path, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0)
        return; /* the old file, unlinked by the rename, keeps the lines rather than losing them */
    close(g_file);
    g_file = fd;
}

static void *copy_thread(void *arg) {
    (void)arg;
    char buf[4096], stamp[40];
    int at_start = 1;
    ssize_t n;
    while ((n = read(g_pipe_read, buf, sizeof(buf))) > 0) {
        log_stamp(stamp, sizeof(stamp));
        log_write_lines(g_file, &at_start, buf, (size_t)n, stamp);
        if (at_start)
            keep_small();
    }
    return NULL;
}

int log_start(const char *path) {
    int fds[2];
    snprintf(g_path, sizeof(g_path), "%s", path);
    log_shrink(path, LOG_MAX, LOG_KEEP);
    g_file = open(path, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (g_file < 0)
        return -1;
    if (pipe(fds)) {
        close(g_file);
        g_file = -1;
        return -1;
    }
    g_pipe_read = fds[0];
    if (pthread_create(&g_thread, NULL, copy_thread, NULL)) {
        /* no thread: the log keeps working, without times */
        close(fds[0]);
        close(fds[1]);
        g_pipe_read = -1;
        fflush(stdout);
        dup2(g_file, STDOUT_FILENO);
        close(g_file);
        g_file = -1;
        return 0;
    }
    g_running = 1;
    fflush(stdout);
    dup2(fds[1], STDOUT_FILENO);
    close(fds[1]);
    return 0;
}

void log_stop(void) {
    if (!g_running)
        return;
    g_running = 0;
    fflush(stdout);
    /* the thread reads the rest, then sees the end once fd 1 no longer holds the pipe.
     * The PS5 refuses close(1) (EPERM), which left the thread waiting and the payload
     * unable to exit: fd 1 is pointed at /dev/null instead. */
    int null = open("/dev/null", O_WRONLY);
    if (null < 0 || dup2(null, STDOUT_FILENO) < 0)
        close(STDOUT_FILENO);
    if (null > STDOUT_FILENO)
        close(null);
    pthread_join(g_thread, NULL);
    close(g_pipe_read);
    close(g_file);
    g_pipe_read = g_file = -1;
}
