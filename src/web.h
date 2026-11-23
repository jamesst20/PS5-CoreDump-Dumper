/* SPDX-License-Identifier: MIT */
#pragma once

#include "log.h"

/*
 * Serve the embedded web UI and the small JSON API on all interfaces.
 * Returns 0 when listening, -1 when the socket could not be opened.
 */
int web_start(unsigned short port, log_t *log);
