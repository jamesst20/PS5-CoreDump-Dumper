#pragma once

#include <stdint.h>

#include "log.h"

typedef struct klog klog_t;

/*
 * Start capturing /dev/klog in a background thread, retaining a rolling
 * window in memory.  Returns NULL when capture cannot be initialized.
 */
klog_t *klog_start(log_t *log);
void klog_stop(klog_t *klog);

/*
 * Write captured log lines in the interval
 * [anchor - before_ms, anchor + after_ms] to path.
 * Returns 0 on success, -1 on error.
 */
int klog_write_around(klog_t *klog, const char *path, int64_t anchor_ms, int before_ms, int after_ms);
