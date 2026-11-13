/* SPDX-License-Identifier: MIT */
#include "dump.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "config.h"
#include "fs.h"

void dump_dest_path(char *path, size_t size, const dump_t *dump, const char *name) {
  if (name) {
    snprintf(path, size, "%s/%s/%s", DUMPER_OUT_ROOT, dump->name, name);
  } else {
    snprintf(path, size, "%s/%s", DUMPER_OUT_ROOT, dump->name);
  }
}

void dump_file_set_name(dump_file_t *file, const char *name) {
  strlcpy(file->name, name, sizeof(file->name));
  file->is_tmp = fs_is_temp_name(name);
  file->is_video = fs_looks_like_video(name);
  file->is_dump = fs_looks_like_dump(name);
}

int dump_file_open_source(const dump_t *dump, const char *name) { return openat(dump->dir_fd, name, O_RDONLY | O_CLOEXEC); }

void dump_file_close(dump_file_t *file, bool sync) {
  if (file->source_fd >= 0) {
    close(file->source_fd);
    file->source_fd = -1;
  }

  if (file->dest_fd >= 0) {
    if (sync) {
      fsync(file->dest_fd);
    }
    close(file->dest_fd);
    file->dest_fd = -1;
  }
}

dump_file_t *dump_find_file(dump_t *dump, const char *name, const struct stat *statbuf) {
  dump_file_t *by_name = NULL;

  for (size_t i = 0; i < dump->file_count; i++) {
    dump_file_t *file = &dump->files[i];

    if (file->dev == statbuf->st_dev && file->ino == statbuf->st_ino) {
      return file;
    }
    if (!strcmp(file->name, name)) {
      by_name = file;
    }
  }

  return by_name;
}

copy_priority_t dump_file_priority(const dump_file_t *file) {
  if (file->is_dump) {
    return COPY_PRIORITY_DUMP;
  }

  return file->is_video ? COPY_PRIORITY_VIDEO : COPY_PRIORITY_OTHER;
}

bool dump_file_is_superseded(const dump_t *dump, const dump_file_t *file) {
  char base[NAME_MAX + 1];

  if (!file->is_tmp) {
    return false;
  }

  strlcpy(base, file->name, sizeof(base));
  fs_strip_temp_suffixes(base);

  for (size_t i = 0; i < dump->file_count; i++) {
    const dump_file_t *other = &dump->files[i];

    if (other != file && !other->is_tmp && !strcasecmp(other->name, base) && other->copied == other->observed &&
        other->observed >= file->observed) {
      return true;
    }
  }

  return false;
}
