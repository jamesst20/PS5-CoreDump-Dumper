/* SPDX-License-Identifier: MIT */
#pragma once

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

/* Order in which files of one coredump are copied. */
typedef enum {
  COPY_PRIORITY_DUMP,
  COPY_PRIORITY_OTHER,
  COPY_PRIORITY_VIDEO,
  COPY_PRIORITY_COUNT,
} copy_priority_t;

/* One source file of a coredump and the state of its copy. */
typedef struct {
  char name[NAME_MAX + 1];
  dev_t dev;
  ino_t ino;
  int source_fd;
  int dest_fd;
  off_t observed;
  off_t copied;
  int64_t last_change_ms;
  bool is_tmp;
  bool is_video;
  bool is_dump;
  bool done;
  bool write_error;
  bool superseded;
} dump_file_t;

/* One coredump folder under the watch directory. */
typedef struct {
  char name[NAME_MAX + 1];
  int dir_fd;
  dev_t dev;
  ino_t ino;
  dump_file_t *files;
  size_t file_count;
  int64_t disappeared_ms;
  bool gone;
  bool drained;
  bool finished;
} dump_t;

/* Build the destination path of a file (or of the dump folder when name is NULL). */
void dump_dest_path(char *path, size_t size, const dump_t *dump, const char *name);

/* Set the file name and the flags derived from it. */
void dump_file_set_name(dump_file_t *file, const char *name);

/* Open the source file relative to the pinned dump directory. */
int dump_file_open_source(const dump_t *dump, const char *name);

/* Release both descriptors, optionally flushing the destination first. */
void dump_file_close(dump_file_t *file, bool sync);

/* Find a file by inode, falling back to its name. */
dump_file_t *dump_find_file(dump_t *dump, const char *name, const struct stat *statbuf);

copy_priority_t dump_file_priority(const dump_file_t *file);

/* True when a temporary file has been fully copied under its final name. */
bool dump_file_is_superseded(const dump_t *dump, const dump_file_t *file);
