/* SPDX-License-Identifier: MIT */
/* Host-side smoke test support for the payload modules. */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

intptr_t kernel_get_root_vnode(void) { return 1; }
intptr_t kernel_get_proc_rootdir(pid_t pid) {
  (void)pid;
  return 2;
}
int32_t kernel_set_proc_rootdir(pid_t pid, intptr_t vnode) {
  (void)pid;
  (void)vnode;
  return 0;
}

int klog_printf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  return 0;
}

int klog_puts(const char *string) {
  puts(string);
  return 0;
}

typedef struct {
  char unused[45];
  char message[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(int user_id, notify_request_t *request, size_t size, int flags) {
  (void)user_id;
  (void)size;
  (void)flags;
  printf("[toast] %s\n", request->message);
  return 0;
}
