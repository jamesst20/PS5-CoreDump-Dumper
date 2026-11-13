/* SPDX-License-Identifier: MIT */
#pragma once

/*
 * The Makefile passes the contents of VERSION on the compiler command line.
 * Host-side builds that bypass the Makefile fall back to this value.
 */
#ifndef DUMPER_VERSION
#define DUMPER_VERSION "unknown"
#endif
