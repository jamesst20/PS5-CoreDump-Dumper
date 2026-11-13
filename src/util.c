/* SPDX-License-Identifier: MIT */
#include "util.h"

#include <errno.h>
#include <time.h>
#include <unistd.h>

int64_t time_monotonic_ms(void) {
  struct timespec ts;

  if (clock_gettime(CLOCK_MONOTONIC, &ts)) {
    return 0;
  }

  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void sleep_ms(int milliseconds) {
  struct timespec delay;

  delay.tv_sec = milliseconds / 1000;
  delay.tv_nsec = (long)(milliseconds % 1000) * 1000000;

  while (nanosleep(&delay, &delay) && errno == EINTR)
    ;
}

bool write_all(int fd, const void *data, size_t length) {
  const uint8_t *bytes = data;

  while (length) {
    ssize_t written = write(fd, bytes, length);

    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }

    bytes += written;
    length -= (size_t)written;
  }

  return true;
}
