/* SPDX-License-Identifier: MIT */
/*
 * Make the payload recognizable in Payload Manager and replace an older
 * instance.  Based on ps5-y2jb-installer's instance handling.
 */
#include "instance.h"

#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <sys/syscall.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>

#include "config.h"

/* Offsets of p_pid and p_comm in a FreeBSD kinfo_proc record. */
#define KI_PID_OFFSET 72
#define KI_COMM_OFFSET 447

#define MAX_ROUNDS 10

void instance_claim_name(void) {
#ifdef SYS_thr_set_name
  syscall(SYS_thr_set_name, -1, DUMPER_INSTANCE_NAME);
#endif
}

static pid_t find_other(void) {
  int mib[4] = {CTL_KERN, KERN_PROC, 8, 0}; /* KERN_PROC_PROC is 8 on FreeBSD */
  pid_t self = getpid();
  pid_t found = -1;
  size_t size = 0;
  uint8_t *buffer;

  if (sysctl(mib, 4, NULL, &size, NULL, 0) != 0) {
    return -2;
  }

  if (!(buffer = malloc(size))) {
    return -2;
  }
  if (sysctl(mib, 4, buffer, &size, NULL, 0) != 0) {
    free(buffer);
    return -2;
  }

  for (uint8_t *pointer = buffer; pointer < buffer + size;) {
    int record;
    pid_t pid;
    const char *command;

    memcpy(&record, pointer, sizeof(record));
    if (record <= 0) {
      break;
    }

    memcpy(&pid, pointer + KI_PID_OFFSET, sizeof(pid));
    command = (const char *)(pointer + KI_COMM_OFFSET);

    if (pid != self && !strcmp(command, DUMPER_INSTANCE_NAME)) {
      found = pid;
    }

    pointer += record;
  }

  free(buffer);

  return found;
}

int instance_take_over(void) {
  int stopped = 0;

  for (int round = 0; round < MAX_ROUNDS; round++) {
    pid_t pid = find_other();

    if (pid == -2) {
      return -1;
    }
    if (pid < 0) {
      return stopped;
    }

    kill(pid, SIGKILL);
    stopped++;
    sleep(1);
  }

  return stopped;
}
