/* SPDX-License-Identifier: MIT */
#include "klog.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/types.h>

#include "config.h"
#include "fs.h"
#include "util.h"

typedef struct {
  int64_t timestamp_ms;
  size_t length;
  char text[];
} klog_line_t;

struct klog {
  log_t *log;
  pthread_t thread;
  pthread_mutex_t mutex;
  volatile bool stopping;
  int fd;
  int64_t last_open_log_ms;
  int64_t retention_ms;
  size_t max_bytes;
  size_t bytes;
  klog_line_t **lines;
  size_t count;
  size_t capacity;
  char partial[4096];
  size_t partial_length;
};

static int64_t time_realtime_ms(void) {
  struct timespec ts;

  if (clock_gettime(CLOCK_REALTIME, &ts)) {
    return 0;
  }

  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void prune_locked(klog_t *klog, int64_t now_ms) {
  int64_t cutoff = now_ms - klog->retention_ms;
  size_t dropped = 0;

  while (dropped < klog->count && (klog->lines[dropped]->timestamp_ms < cutoff || klog->bytes > klog->max_bytes)) {
    klog->bytes -= klog->lines[dropped]->length;
    free(klog->lines[dropped]);
    dropped++;
  }

  if (dropped) {
    memmove(klog->lines, klog->lines + dropped, (klog->count - dropped) * sizeof(*klog->lines));
    klog->count -= dropped;
  }
}

static void append_line(klog_t *klog, const char *text, size_t length) {
  int64_t now_ms = time_monotonic_ms();
  klog_line_t *line;

  while (length && (text[length - 1] == '\r' || text[length - 1] == '\n')) {
    length--;
  }
  if (length == 0) {
    return;
  }

  if (klog->count == klog->capacity) {
    size_t capacity = klog->capacity ? klog->capacity * 2 : 256;
    klog_line_t **lines = realloc(klog->lines, capacity * sizeof(*lines));

    if (!lines) {
      return;
    }
    klog->lines = lines;
    klog->capacity = capacity;
  }

  line = malloc(sizeof(*line) + length + 1);
  if (!line) {
    return;
  }

  line->timestamp_ms = now_ms;
  line->length = length;
  memcpy(line->text, text, length);
  line->text[length] = '\0';

  pthread_mutex_lock(&klog->mutex);
  prune_locked(klog, now_ms);
  klog->bytes += length;
  klog->lines[klog->count++] = line;
  prune_locked(klog, now_ms);
  pthread_mutex_unlock(&klog->mutex);
}

static void consume(klog_t *klog, const char *data, size_t length) {
  for (size_t i = 0; i < length; i++) {
    if (data[i] == '\n') {
      append_line(klog, klog->partial, klog->partial_length);
      klog->partial_length = 0;
    } else if (klog->partial_length < sizeof(klog->partial) - 1) {
      klog->partial[klog->partial_length++] = data[i];
    } else {
      /* A line longer than the buffer is flushed as its own line. */
      append_line(klog, klog->partial, klog->partial_length);
      klog->partial_length = 0;
    }
  }
}

static void *capture_thread(void *argument) {
  klog_t *klog = argument;
  char buffer[8192];

  while (!klog->stopping) {
    ssize_t count;

    if (klog->fd < 0) {
      klog->fd = fs_open_root(DUMPER_KLOG_PATH, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
      if (klog->fd < 0) {
        int64_t now_ms = time_monotonic_ms();

        if (now_ms - klog->last_open_log_ms >= 5000) {
          log_printf(klog->log, "cannot open %s: %s", DUMPER_KLOG_PATH, strerror(errno));
          klog->last_open_log_ms = now_ms;
        }
        sleep_ms(500);
        continue;
      }
    }

    count = read(klog->fd, buffer, sizeof(buffer));
    if (count > 0) {
      consume(klog, buffer, (size_t)count);
    } else if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
      close(klog->fd);
      klog->fd = -1;
      sleep_ms(100);
    } else {
      sleep_ms(20);
    }
  }

  return NULL;
}

klog_t *klog_start(log_t *log) {
  klog_t *klog;

  if (!(klog = calloc(1, sizeof(*klog)))) {
    return NULL;
  }

  klog->log = log;
  klog->fd = -1;
  klog->retention_ms = DUMPER_KLOG_RETENTION_MS;
  klog->max_bytes = DUMPER_KLOG_MAX_BYTES;

  if (pthread_mutex_init(&klog->mutex, NULL)) {
    free(klog);
    return NULL;
  }

  if (pthread_create(&klog->thread, NULL, capture_thread, klog)) {
    pthread_mutex_destroy(&klog->mutex);
    free(klog);
    return NULL;
  }

  log_printf(log, "kernel log capture started");

  return klog;
}

void klog_stop(klog_t *klog) {
  if (!klog) {
    return;
  }

  klog->stopping = true;
  pthread_join(klog->thread, NULL);

  if (klog->fd >= 0) {
    close(klog->fd);
  }

  for (size_t i = 0; i < klog->count; i++) {
    free(klog->lines[i]);
  }

  free(klog->lines);
  pthread_mutex_destroy(&klog->mutex);
  free(klog);
}

int klog_write_around(klog_t *klog, const char *path, int64_t anchor_ms, int before_ms, int after_ms) {
  struct timespec realtime;
  char header[256];
  char stamp[64];
  char prefix[96];
  struct tm tm;
  int64_t now_ms;
  int64_t start_ms;
  int64_t end_ms;
  int64_t wall_now_ms;
  int fd;
  int length;

  if (!klog) {
    return -1;
  }

  now_ms = time_monotonic_ms();
  wall_now_ms = time_realtime_ms();
  start_ms = anchor_ms - before_ms;
  end_ms = anchor_ms + after_ms;
  if (end_ms > now_ms) {
    end_ms = now_ms;
  }

  fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, DUMPER_MODE_FILE);
  if (fd < 0) {
    return -1;
  }

  clock_gettime(CLOCK_REALTIME, &realtime);
  localtime_r(&realtime.tv_sec, &tm);
  strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);
  length = snprintf(header, sizeof(header),
                    "# kernel log: %dms before / %dms after coredump disappearance\n"
                    "# timestamps are prefixed by the dumper when the line was read;\n"
                    "# /dev/klog itself does not timestamp its lines\n"
                    "# captured until %s.%03ld\n\n",
                    before_ms, after_ms, stamp, realtime.tv_nsec / 1000000);
  write_all(fd, header, (size_t)length);

  pthread_mutex_lock(&klog->mutex);

  for (size_t i = 0; i < klog->count; i++) {
    klog_line_t *line = klog->lines[i];
    int64_t wall_ms;
    time_t wall_seconds;

    if (line->timestamp_ms < start_ms) {
      continue;
    }
    if (line->timestamp_ms > end_ms) {
      break;
    }

    wall_ms = wall_now_ms - (now_ms - line->timestamp_ms);
    wall_seconds = (time_t)(wall_ms / 1000);
    localtime_r(&wall_seconds, &tm);
    strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm);

    length = snprintf(prefix, sizeof(prefix), "[%s.%03lld] ", stamp, (long long)(wall_ms % 1000));
    write_all(fd, prefix, (size_t)length);
    write_all(fd, line->text, line->length);
    write_all(fd, "\n", 1);
  }

  pthread_mutex_unlock(&klog->mutex);

  fsync(fd);
  close(fd);

  return 0;
}
