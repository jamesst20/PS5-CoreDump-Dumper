/* SPDX-License-Identifier: MIT */
#include "notify.h"

#include <stddef.h>
#include <string.h>

typedef struct {
  char unused[45];
  char message[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(int user_id, notify_request_t *request, size_t size, int flags);

void notify_send(const char *message) {
  notify_request_t request;

  memset(&request, 0, sizeof(request));
  strncpy(request.message, message, sizeof(request.message) - 1);

  sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
}
