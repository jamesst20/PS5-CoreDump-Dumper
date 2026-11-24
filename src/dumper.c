/* SPDX-License-Identifier: MIT */
#include "dumper.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "dump.h"
#include "fs.h"
#include "util.h"

struct dumper {
  log_t *log;
  klog_t *klog;
  int watch_fd;
  dump_t *dumps;
  size_t dump_count;
  char (*remembered)[NAME_MAX + 1];
  size_t remembered_next;
  uint8_t *buffer;
};

static void remember_name(dumper_t *dumper, const char *name) {
  strlcpy(dumper->remembered[dumper->remembered_next], name, NAME_MAX + 1);
  dumper->remembered_next = (dumper->remembered_next + 1) % DUMPER_MAX_REMEMBERED;
}

static bool is_remembered(const dumper_t *dumper, const char *name) {
  for (size_t i = 0; i < DUMPER_MAX_REMEMBERED; i++) {
    if (!strcmp(dumper->remembered[i], name)) {
      return true;
    }
  }

  return false;
}

static dump_t *find_dump(dumper_t *dumper, const char *name) {
  for (size_t i = 0; i < dumper->dump_count; i++) {
    if (!strcmp(dumper->dumps[i].name, name)) {
      return &dumper->dumps[i];
    }
  }

  return NULL;
}

/* Resume a copy left behind by a previous run instead of starting over. */
static void resume_existing_copy(dump_file_t *file, const char *path, off_t source_size) {
  struct stat statbuf;

  if (stat(path, &statbuf) || !S_ISREG(statbuf.st_mode)) {
    return;
  }

  if (statbuf.st_size == source_size) {
    file->copied = source_size;
    file->done = true;
  } else if (statbuf.st_size < source_size) {
    file->copied = statbuf.st_size;
  }
}

static dump_file_t *adopt_file(dumper_t *dumper, dump_t *dump, const char *name, const struct stat *statbuf) {
  char path[PATH_MAX];
  dump_file_t *file;

  if (dump->file_count >= DUMPER_MAX_FILES) {
    log_printf(dumper->log, "%s: too many files, ignoring %s", dump->name, name);
    return NULL;
  }

  file = &dump->files[dump->file_count++];
  memset(file, 0, sizeof(*file));
  dump_file_set_name(file, name);
  file->dev = statbuf->st_dev;
  file->ino = statbuf->st_ino;
  file->observed = statbuf->st_size;
  file->source_fd = -1;
  file->dest_fd = -1;
  file->last_change_ms = time_monotonic_ms();

  dump_dest_path(path, sizeof(path), dump, name);
  resume_existing_copy(file, path, statbuf->st_size);

  file->source_fd = dump_file_open_source(dump, name);
  if (file->source_fd < 0) {
    log_printf(dumper->log, "%s: cannot pin %s: %s", dump->name, name, strerror(errno));
  }

  log_printf(dumper->log, "%s: %s (%lld bytes)%s", dump->name, name, (long long)statbuf->st_size, file->is_tmp ? " [tmp]" : "");

  return file;
}

static void retarget_dest(dump_t *dump, dump_file_t *file, const char *name) {
  char old_path[PATH_MAX];
  char new_path[PATH_MAX];

  if (!strcmp(file->name, name)) {
    return;
  }

  dump_dest_path(old_path, sizeof(old_path), dump, file->name);
  dump_dest_path(new_path, sizeof(new_path), dump, name);

  if (file->dest_fd >= 0) {
    close(file->dest_fd);
    file->dest_fd = -1;
    rename(old_path, new_path);
  }

  dump_file_set_name(file, name);
}

static void scan_dump(dumper_t *dumper, dump_t *dump) {
  struct dirent *entry;
  struct stat statbuf;
  int scan_fd;
  DIR *dir;

  scan_fd = openat(dump->dir_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (scan_fd < 0) {
    /* A deleted directory can still be read through the pinned fd. */
    scan_fd = dup(dump->dir_fd);
  }
  if (scan_fd < 0 || !(dir = fdopendir(scan_fd))) {
    if (scan_fd >= 0) {
      close(scan_fd);
    }
    return;
  }

  while ((entry = readdir(dir))) {
    dump_file_t *file;

    if (!fs_safe_name(entry->d_name)) {
      continue;
    }
    if (fstatat(dump->dir_fd, entry->d_name, &statbuf, AT_SYMLINK_NOFOLLOW)) {
      continue;
    }
    if (!S_ISREG(statbuf.st_mode)) {
      continue;
    }

    file = dump_find_file(dump, entry->d_name, &statbuf);
    if (file && (file->ino != statbuf.st_ino || file->dev != statbuf.st_dev)) {
      dump_file_close(file, false);
      file->copied = 0;
      file->done = false;
      file->write_error = false;
      file->dev = statbuf.st_dev;
      file->ino = statbuf.st_ino;
      dump_file_set_name(file, entry->d_name);
      file->source_fd = dump_file_open_source(dump, entry->d_name);
    } else if (file && strcmp(file->name, entry->d_name)) {
      /* The temporary suffix was dropped: the file is ready. */
      log_printf(dumper->log, "%s: ready %s", dump->name, entry->d_name);
      retarget_dest(dump, file, entry->d_name);
    } else if (!file) {
      file = adopt_file(dumper, dump, entry->d_name, &statbuf);
    }

    if (!file) {
      continue;
    }

    if (file->observed != statbuf.st_size) {
      file->observed = statbuf.st_size;
      file->last_change_ms = time_monotonic_ms();
      file->done = false;
    }

    if (file->source_fd < 0) {
      file->source_fd = dump_file_open_source(dump, entry->d_name);
    }
  }

  closedir(dir);
}

static int ensure_dest_fd(dumper_t *dumper, dump_t *dump, dump_file_t *file) {
  char path[PATH_MAX];

  if (file->dest_fd >= 0) {
    return 0;
  }

  dump_dest_path(path, sizeof(path), dump, file->name);
  file->dest_fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC, DUMPER_MODE_FILE);
  if (file->dest_fd < 0) {
    if (!file->write_error) {
      log_printf(dumper->log, "%s: cannot create %s: %s", dump->name, path, strerror(errno));
      file->write_error = true;
    }
    return -1;
  }

  if (ftruncate(file->dest_fd, file->copied)) {
    log_printf(dumper->log, "%s: ftruncate %s: %s", dump->name, path, strerror(errno));
  }

  return 0;
}

/* Reopen the pinned source after a transient devfs error, or give up. */
static bool reopen_source(dump_t *dump, dump_file_t *file, int *retries) {
  if (file->source_fd >= 0) {
    close(file->source_fd);
    file->source_fd = -1;
  }

  if ((*retries)++ < DUMPER_READ_RETRIES) {
    sleep_ms(DUMPER_READ_RETRY_MS);
    file->source_fd = dump_file_open_source(dump, file->name);
    return true;
  }

  file->done = true;

  return false;
}

/*
 * Copy at most DUMPER_COPY_CHUNK bytes.  In drain mode a pinned file is read
 * to its final size; otherwise only up to the currently observed size.
 */
static size_t copy_file(dumper_t *dumper, dump_t *dump, dump_file_t *file, bool drain) {
  size_t budget = DUMPER_COPY_CHUNK;
  int retries = 0;

  if (file->done) {
    return 0;
  }

  if (file->source_fd < 0 && (file->source_fd = dump_file_open_source(dump, file->name)) < 0) {
    return 0;
  }
  if (ensure_dest_fd(dumper, dump, file)) {
    return 0;
  }

  while (budget) {
    size_t wanted = budget < DUMPER_READ_CHUNK ? budget : DUMPER_READ_CHUNK;
    ssize_t count;

    if (file->source_fd < 0 && !reopen_source(dump, file, &retries)) {
      break;
    }

    if (!drain) {
      off_t pending = file->observed - file->copied;

      if (pending <= 0) {
        break;
      }
      if ((off_t)wanted > pending) {
        wanted = (size_t)pending;
      }
    }

    if (lseek(file->source_fd, file->copied, SEEK_SET) < 0) {
      log_printf(dumper->log, "%s: seek %s to %lld: %s", dump->name, file->name, (long long)file->copied, strerror(errno));
      file->done = true;
      break;
    }

    count = read(file->source_fd, dumper->buffer, wanted);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      /* devfs sometimes rejects an access on an actively written file; a
       * fresh descriptor after a short pause usually works. */
      if (reopen_source(dump, file, &retries)) {
        continue;
      }
      log_printf(dumper->log, "%s: read %s at %lld: %s", dump->name, file->name, (long long)file->copied, strerror(errno));
      break;
    }
    if (count == 0) {
      struct stat statbuf;

      if (!drain) {
        break;
      }
      if (fstat(file->source_fd, &statbuf) == 0 && statbuf.st_size > file->copied) {
        file->observed = statbuf.st_size;
        file->last_change_ms = time_monotonic_ms();
        continue;
      }
      if (time_monotonic_ms() - file->last_change_ms >= DUMPER_STABLE_MS) {
        file->done = true;
        break;
      }

      sleep_ms(DUMPER_READ_RETRY_MS);
      continue;
    }

    for (ssize_t offset = 0; offset < count;) {
      ssize_t written;

      if (lseek(file->dest_fd, file->copied + offset, SEEK_SET) < 0) {
        log_printf(dumper->log, "%s: seek dest %s: %s", dump->name, file->name, strerror(errno));
        file->done = true;
        return 0;
      }

      written = write(file->dest_fd, dumper->buffer + offset, (size_t)(count - offset));

      if (written < 0) {
        if (errno == EINTR) {
          continue;
        }
        if (!file->write_error) {
          log_printf(dumper->log, "%s: write %s: %s", dump->name, file->name, strerror(errno));
          file->write_error = true;
        }
        file->done = true;
        return 0;
      }
      offset += written;
    }

    file->copied += count;
    budget -= (size_t)count;
    retries = 0;

    if (drain) {
      struct stat statbuf;

      if (fstat(file->source_fd, &statbuf) == 0 && statbuf.st_size != file->observed) {
        file->observed = statbuf.st_size;
        file->last_change_ms = time_monotonic_ms();
      }
      if (file->copied >= file->observed && time_monotonic_ms() - file->last_change_ms >= DUMPER_STABLE_MS) {
        file->done = true;
        break;
      }
    }
  }

  if (!file->done && !drain && !file->is_tmp && file->copied >= file->observed &&
      time_monotonic_ms() - file->last_change_ms >= DUMPER_STABLE_MS) {
    file->done = true;
  }

  return DUMPER_COPY_CHUNK - budget;
}

static size_t copy_pass(dumper_t *dumper, dump_t *dump, bool drain) {
  size_t total = 0;

  for (copy_priority_t priority = COPY_PRIORITY_DUMP; priority < COPY_PRIORITY_COUNT; priority++) {
    for (size_t i = 0; i < dump->file_count; i++) {
      dump_file_t *file = &dump->files[i];

      if (dump_file_priority(file) != priority || file->done) {
        continue;
      }
      if (!drain && file->is_tmp) {
        continue;
      }
      if (drain && dump_file_is_superseded(dump, file)) {
        file->done = true;
        file->superseded = true;
        continue;
      }

      total += copy_file(dumper, dump, file, drain);
    }
  }

  return total;
}

static const char *file_status(const dump_file_t *file) {
  if (file->superseded) {
    return "superseded by final name";
  }
  if (file->copied == file->observed && !file->is_tmp) {
    return "ok";
  }
  if (file->copied == file->observed) {
    return "tmp (never renamed)";
  }

  return "partial";
}

static void write_manifest(dump_t *dump) {
  char path[PATH_MAX];
  char line[PATH_MAX + 128];
  int fd;
  int length;

  dump_dest_path(path, sizeof(path), dump, "_manifest.txt");
  fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, DUMPER_MODE_FILE);
  if (fd < 0) {
    return;
  }

  length = snprintf(line, sizeof(line), "source: %s/%s\nfiles: %zu\nkernel_log: _klog.txt\n\n", DUMPER_WATCH_DIR, dump->name,
                    dump->file_count);
  write_all(fd, line, (size_t)length);

  for (size_t i = 0; i < dump->file_count; i++) {
    dump_file_t *file = &dump->files[i];

    length = snprintf(line, sizeof(line), "%s\t%lld/%lld\t%s\n", file->name, (long long)file->copied, (long long)file->observed,
                      file_status(file));
    write_all(fd, line, (size_t)length);
  }

  close(fd);
}

static void write_kernel_log(dumper_t *dumper, dump_t *dump) {
  static const char unavailable[] = "# kernel log capture was not available\n";
  char path[PATH_MAX];
  int64_t anchor_ms;

  dump_dest_path(path, sizeof(path), dump, "_klog.txt");
  anchor_ms = dump->disappeared_ms ? dump->disappeared_ms : time_monotonic_ms();

  if (dumper->klog) {
    if (klog_write_around(dumper->klog, path, anchor_ms, DUMPER_KLOG_RETENTION_MS, DUMPER_KLOG_GRACE_MS)) {
      log_printf(dumper->log, "%s: cannot write %s: %s", dump->name, path, strerror(errno));
    }
    return;
  }

  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, DUMPER_MODE_FILE);
  if (fd >= 0) {
    write_all(fd, unavailable, sizeof(unavailable) - 1);
    close(fd);
  }
}

static void write_ampr_trace(dump_t *dump) {
  static const char path[] = "/data/ampr_emu_trace.txt";
  char dest[PATH_MAX];
  char chunk[65536];
  int in;
  int out;

  in = open(path, O_RDONLY | O_CLOEXEC);
  if (in < 0) {
    return;
  }
  dump_dest_path(dest, sizeof(dest), dump, "_ampr_trace.txt");
  out = open(dest, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, DUMPER_MODE_FILE);
  if (out >= 0) {
    for (;;) {
      ssize_t got = read(in, chunk, sizeof(chunk));
      if (got < 0 && errno == EINTR) {
        continue;
      }
      if (got <= 0 || !write_all(out, chunk, (size_t)got)) {
        break;
      }
    }
    close(out);
  }
  close(in);
}

static void finish_dump(dumper_t *dumper, dump_t *dump) {
  write_manifest(dump);
  write_kernel_log(dumper, dump);
  write_ampr_trace(dump);
  log_printf(dumper->log, "%s: finished, %zu file(s) tracked", dump->name, dump->file_count);

  for (size_t i = 0; i < dump->file_count; i++) {
    dump_file_close(&dump->files[i], true);
  }

  close(dump->dir_fd);
  free(dump->files);
  dump->files = NULL;
  dump->finished = true;
}

static void prune_dumps(dumper_t *dumper) {
  size_t kept = 0;

  for (size_t i = 0; i < dumper->dump_count; i++) {
    if (dumper->dumps[i].finished) {
      continue;
    }
    if (kept != i) {
      dumper->dumps[kept] = dumper->dumps[i];
    }
    kept++;
  }

  dumper->dump_count = kept;
}

static bool dump_still_exists(const dumper_t *dumper, const dump_t *dump) {
  struct stat statbuf;

  if (dumper->watch_fd < 0) {
    return true;
  }

  if (fstatat(dumper->watch_fd, dump->name, &statbuf, AT_SYMLINK_NOFOLLOW)) {
    return errno != ENOENT;
  }

  return statbuf.st_dev == dump->dev && statbuf.st_ino == dump->ino;
}

static void start_dump(dumper_t *dumper, const char *name) {
  char path[PATH_MAX];
  struct stat statbuf;
  dump_t *dump;
  int dir_fd;

  if (dumper->dump_count >= DUMPER_MAX_ACTIVE) {
    log_printf(dumper->log, "too many active coredumps, ignoring %s", name);
    return;
  }

  dir_fd = openat(dumper->watch_fd, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dir_fd < 0) {
    return;
  }
  if (fstat(dir_fd, &statbuf)) {
    close(dir_fd);
    return;
  }

  dump = &dumper->dumps[dumper->dump_count++];
  memset(dump, 0, sizeof(*dump));
  strlcpy(dump->name, name, NAME_MAX + 1);
  dump->dir_fd = dir_fd;
  dump->dev = statbuf.st_dev;
  dump->ino = statbuf.st_ino;
  dump->files = calloc(DUMPER_MAX_FILES, sizeof(dump_file_t));
  if (!dump->files) {
    close(dump->dir_fd);
    dumper->dump_count--;
    return;
  }

  dump_dest_path(path, sizeof(path), dump, NULL);
  if (mkdir(path, DUMPER_MODE_DIR) && errno != EEXIST) {
    log_printf(dumper->log, "%s: mkdir %s: %s", name, path, strerror(errno));
  }

  log_printf(dumper->log, "%s: coredump detected", name);
  remember_name(dumper, name);
  scan_dump(dumper, dump);
}

static int open_watch_dir(dumper_t *dumper) {
  if (dumper->watch_fd >= 0) {
    return 0;
  }

  dumper->watch_fd = fs_open_root(DUMPER_WATCH_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dumper->watch_fd < 0) {
    return -1;
  }

  log_printf(dumper->log, "watching %s", DUMPER_WATCH_DIR);

  return 0;
}

static void scan_watch_dir(dumper_t *dumper) {
  struct dirent *entry;
  struct stat statbuf;
  int scan_fd;
  DIR *dir;

  if (open_watch_dir(dumper)) {
    return;
  }

  scan_fd = openat(dumper->watch_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (scan_fd < 0) {
    if (errno == ENOENT) {
      close(dumper->watch_fd);
      dumper->watch_fd = -1;
    }
    return;
  }

  if (!(dir = fdopendir(scan_fd))) {
    close(scan_fd);
    return;
  }

  while ((entry = readdir(dir))) {
    if (!fs_safe_name(entry->d_name) || !strchr(entry->d_name, '_')) {
      continue;
    }
    if (find_dump(dumper, entry->d_name) || is_remembered(dumper, entry->d_name)) {
      continue;
    }
    if (fstatat(dumper->watch_fd, entry->d_name, &statbuf, AT_SYMLINK_NOFOLLOW)) {
      continue;
    }
    if (!S_ISDIR(statbuf.st_mode)) {
      continue;
    }

    start_dump(dumper, entry->d_name);
  }

  closedir(dir);
}

/* Read every pinned file to completion once the source folder is gone. */
static void drain_dump(dumper_t *dumper, dump_t *dump) {
  for (size_t spin = 0; spin < 100000; spin++) {
    if (copy_pass(dumper, dump, true)) {
      continue;
    }

    for (size_t i = 0; i < dump->file_count; i++) {
      dump_file_t *file = &dump->files[i];

      if (!file->done) {
        log_printf(dumper->log, "%s: giving up on %s at %lld/%lld", dump->name, file->name, (long long)file->copied,
                   (long long)file->observed);
        file->done = true;
      }
    }
    break;
  }
}

dumper_t *dumper_create(log_t *log, klog_t *klog) {
  dumper_t *dumper;

  if (!(dumper = calloc(1, sizeof(*dumper)))) {
    return NULL;
  }

  dumper->log = log;
  dumper->klog = klog;
  dumper->watch_fd = -1;
  dumper->buffer = malloc(DUMPER_COPY_CHUNK);
  dumper->dumps = calloc(DUMPER_MAX_ACTIVE, sizeof(dump_t));
  dumper->remembered = calloc(DUMPER_MAX_REMEMBERED, NAME_MAX + 1);

  if (!dumper->buffer || !dumper->dumps || !dumper->remembered) {
    dumper_destroy(dumper);
    return NULL;
  }

  return dumper;
}

void dumper_destroy(dumper_t *dumper) {
  if (!dumper) {
    return;
  }

  for (size_t i = 0; i < dumper->dump_count; i++) {
    dump_t *dump = &dumper->dumps[i];

    if (dump->finished) {
      continue;
    }
    drain_dump(dumper, dump);
    finish_dump(dumper, dump);
  }

  if (dumper->watch_fd >= 0) {
    close(dumper->watch_fd);
  }

  free(dumper->remembered);
  free(dumper->dumps);
  free(dumper->buffer);
  free(dumper);
}

bool dumper_poll(dumper_t *dumper) {
  scan_watch_dir(dumper);

  for (size_t i = 0; i < dumper->dump_count; i++) {
    dump_t *dump = &dumper->dumps[i];

    if (dump->finished) {
      continue;
    }

    if (!dump->gone && !dump_still_exists(dumper, dump)) {
      log_printf(dumper->log, "%s: directory disappeared, draining and collecting kernel log", dump->name);
      dump->gone = true;
      dump->disappeared_ms = time_monotonic_ms();
      scan_dump(dumper, dump);
    }

    if (dump->gone) {
      if (!dump->drained) {
        drain_dump(dumper, dump);
        dump->drained = true;
      }
      if (time_monotonic_ms() - dump->disappeared_ms >= DUMPER_KLOG_GRACE_MS) {
        finish_dump(dumper, dump);
      }
    } else {
      scan_dump(dumper, dump);
      copy_pass(dumper, dump, false);
    }
  }

  prune_dumps(dumper);

  return dumper->dump_count > 0;
}
