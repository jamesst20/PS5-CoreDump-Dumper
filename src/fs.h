/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>

/*
 * Open a path relative to the filesystem root.  Payload sandboxes do not
 * always expose /devlog, so fall back to temporarily switching the process
 * root (the same technique used by the SDK's list_files sample).
 */
int fs_open_root(const char *path, int flags);

/* Validate a directory entry name for safe use in a destination path. */
bool fs_safe_name(const char *name);

bool fs_has_suffix(const char *string, const char *suffix);

/* Temporary files are written as "<name>.tmp" or "<name>.part" and renamed when complete. */
bool fs_is_temp_name(const char *name);
void fs_strip_temp_suffixes(char *name);

bool fs_looks_like_video(const char *name);
bool fs_looks_like_dump(const char *name);
