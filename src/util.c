/* SPDX-License-Identifier: MIT */
#include "util.h"

#include <errno.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>

int64_t time_monotonic_ms(void) {
  struct timespec ts;

  if (clock_gettime(CLOCK_MONOTONIC, &ts)) {
    return 0;
  }

  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

bool local_ipv4(char *buffer, size_t size) {
  struct ifaddrs *interfaces = NULL;
  bool found = false;

  if (getifaddrs(&interfaces)) {
    return false;
  }
  for (struct ifaddrs *entry = interfaces; entry; entry = entry->ifa_next) {
    const struct sockaddr_in *address;

    if (!entry->ifa_addr || entry->ifa_addr->sa_family != AF_INET || !(entry->ifa_flags & IFF_UP)) {
      continue;
    }
    address = (const struct sockaddr_in *)entry->ifa_addr;
    if (address->sin_addr.s_addr == htonl(INADDR_LOOPBACK) || address->sin_addr.s_addr == 0) {
      continue;
    }
    if (inet_ntop(AF_INET, &address->sin_addr, buffer, (socklen_t)size)) {
      found = true;
      break;
    }
  }
  freeifaddrs(interfaces);
  return found;
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
