/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdarg.h>

typedef struct {
  int fd;
} log_t;

void log_init(log_t *log, const char *path);
void log_close(log_t *log);
void log_printf(log_t *log, const char *fmt, ...);
