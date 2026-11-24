/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/*
 * Read-only ampr-emu-ng debug helpers. The payload is only the transport:
 * it reads the trace and proxies the debug build's 9090 pages.
 */
void web_debug_route(int fd, const char *method, const char *path, const char *query, bool csrf);
