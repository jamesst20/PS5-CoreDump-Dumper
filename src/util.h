/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

int64_t time_monotonic_ms(void);
void sleep_ms(int milliseconds);

/* Write the whole buffer, retrying on EINTR.  Returns false on a real error. */
bool write_all(int fd, const void *data, size_t length);
