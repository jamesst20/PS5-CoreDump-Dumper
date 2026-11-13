/* SPDX-License-Identifier: MIT */
#include "log.h"

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef HOST_TEST
int klog_printf(const char *fmt, ...);
#else
#include <ps5/klog.h>
#endif

#include "config.h"
#include "util.h"

void log_init(log_t *log, const char *path) { log->fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, DUMPER_MODE_FILE); }

void log_close(log_t *log) {
  if (log->fd >= 0) {
    close(log->fd);
    log->fd = -1;
  }
}

void log_printf(log_t *log, const char *fmt, ...) {
  struct timespec ts;
  struct tm tm;
  char line[1024];
  char stamp[32];
  va_list ap;
  int length;

  if (!log) {
    return;
  }

  clock_gettime(CLOCK_REALTIME, &ts);
  localtime_r(&ts.tv_sec, &tm);
  strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);

  va_start(ap, fmt);
  length = snprintf(line, sizeof(line), "[%s.%03ld] ", stamp, ts.tv_nsec / 1000000);
  vsnprintf(line + length, sizeof(line) - (size_t)length, fmt, ap);
  va_end(ap);

  klog_printf("%s\n", line);

  if (log->fd >= 0) {
    write_all(log->fd, line, strlen(line));
    write_all(log->fd, "\n", 1);
  }
}
