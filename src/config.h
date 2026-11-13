/* SPDX-License-Identifier: MIT */
#pragma once

/*
 * Compile-time configuration.  Paths can be overridden from the command line
 * (the host smoke test does that to run in a temporary directory).
 */

#ifndef DUMPER_WATCH_DIR
#define DUMPER_WATCH_DIR "/devlog/system/sce_coredumps.0"
#endif

#ifndef DUMPER_OUT_ROOT
#define DUMPER_OUT_ROOT "/data/ps5-coredump-dumper"
#endif

#ifndef DUMPER_KLOG_PATH
#define DUMPER_KLOG_PATH "/dev/klog"
#endif

/* The kernel keeps 19 characters.  The .elf suffix makes Payload Manager
 * list this as a user process. */
#define DUMPER_INSTANCE_NAME "coredump-dump.elf"

#define DUMPER_LOCK_PATH DUMPER_OUT_ROOT "/dumper.lock"
#define DUMPER_LOG_PATH DUMPER_OUT_ROOT "/dumper.log"

/* The console normally deletes a coredump folder within seconds. */
#define DUMPER_IDLE_MS 100
#define DUMPER_ACTIVE_MS 25
#define DUMPER_STABLE_MS 900

/* Keep the kernel log from one minute before until three seconds after a
 * coredump folder disappears. */
#define DUMPER_KLOG_RETENTION_MS 60000
#define DUMPER_KLOG_GRACE_MS 3000
#define DUMPER_KLOG_MAX_BYTES (8u * 1024u * 1024u)

#define DUMPER_COPY_CHUNK (8u * 1024u * 1024u)
#define DUMPER_READ_CHUNK (64u * 1024u)

#define DUMPER_MAX_ACTIVE 16u
#define DUMPER_MAX_FILES 256u
#define DUMPER_MAX_REMEMBERED 128u

/* devfs can briefly reject reads on a file another process is still
 * writing.  Retry with a fresh descriptor before giving up. */
#define DUMPER_READ_RETRIES 100
#define DUMPER_READ_RETRY_MS 10

#define DUMPER_MODE_FILE 0644
#define DUMPER_MODE_DIR 0755
