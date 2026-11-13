/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>

#include "klog.h"
#include "log.h"

typedef struct dumper dumper_t;

dumper_t *dumper_create(log_t *log, klog_t *klog);
void dumper_destroy(dumper_t *dumper);

/*
 * Run one poll cycle.  Returns true while at least one coredump is being
 * tracked, so callers know whether to use the active poll interval.
 */
bool dumper_poll(dumper_t *dumper);
